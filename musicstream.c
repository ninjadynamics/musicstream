/*
 * musicstream.irx — custom IOP music streamer for SuperSolar (PS2).
 *
 * Replaces the EE-driven streamer: the EE only sends play/stop/vol/pause over
 * SIF RPC; everything else — reading the .adpcm and feeding SPU2 — runs here on
 * the IOP, so the EE does ZERO per-frame audio work (structurally stall-free).
 *
 * Design (see PS2/IOP_AUDIO_PLAN.md, PS2/LESSONS.md):
 *   - Stereo = two SPU2 voices (22 L, 23 R), each a 2-half ring in SPU2 RAM.
 *     The .adpcm is chunk-interleaved [L:CHUNK][R:CHUNK]; one CHUNK == one half.
 *   - A dedicated stream thread polls the play cursor (NAX) every ~8 ms and,
 *     when the cursor crosses into a new half, refills the half it just left
 *     with the next file chunk. The ring self-loops via the SPU block flags
 *     (0x06 start / 0x03 end), so playback is gapless; the file rewinds at EOF.
 *   - THE RULE that beats ps2snd: the blocking file read + SPU DMA happen ONLY
 *     on the stream thread, never inside an IRQ or the RPC handler. The RPC
 *     handler just hands a command to the stream thread and waits for the ack.
 *   - Uses the SAME libsd (sceSd*) the SFX use (voices 0-11). We never call
 *     sceSdInit — the EE already initialized SPU2 via ps2snd; we only drive
 *     our two voices.
 *
 * IOP note: the R3000A IOP has no write-back data cache (its D-cache is the
 * scratchpad), so CPU writes to the read buffer are visible to the SPU DMA with
 * no flush — unlike the EE, which needed SyncDCache + an IOP bounce buffer.
 */
#include <types.h>
#include <stdio.h>
#include <sysclib.h>   /* memcpy / memset */
#include <thbase.h>
#include <thsemap.h>
#include <sifcmd.h>
#include <loadcore.h>
#include <libsd.h>
#include <iomanX.h>

#include "musicstream_rpc.h"

IRX_ID("supersolar_music", 1, 0);

/* ---- Ring geometry (MUST match Makefile PS2_MUSIC_CHUNK and the EE side) ---- */
#define MUS_CHUNK_BLOCKS  2048
#define MUS_HALF_BYTES    (MUS_CHUNK_BLOCKS * 16)        /* 32 KB per channel-half */
#define MUS_RING_BYTES    (MUS_HALF_BYTES * 2)           /* 64 KB per channel ring */
#define MUS_L_RING        0x1A0000u
#define MUS_R_RING        (MUS_L_RING + MUS_RING_BYTES)  /* 0x1B0000 */
#define MUS_VOICE_L       22
#define MUS_VOICE_R       23

#define FIO_RDONLY        0x0001
#define FIO_SEEK_SET      0

/* ---- State (all owned/mutated by the stream thread; see handshake below) ---- */
static int  g_fd        = -1;
static int  g_loop      = 1;
static int  g_playing   = 0;
static int  g_paused    = 0;
static int  g_vol       = 0x3fff;
static int  g_last_half = 0;

/* Async open: a PLAY command only *requests* playback (so the EE RPC returns at
   once instead of blocking on the ~128 KB open+prime). The stream thread does the
   actual open in its loop and self-retries on failure (USB mount latency), so the
   EE never blocks and never has to retry. */
static int  g_want_play  = 0;            /* should be playing g_play_path        */
static int  g_play_loop  = 1;
static int  g_open_div   = 0;            /* ticks until the next open attempt     */
static char g_play_path[256];
static unsigned char g_buf[MUS_HALF_BYTES * 2] __attribute__((aligned(64)));  /* one [L][R] chunk */

/* ---- EE command handshake (RPC thread -> stream thread) ----
   The EE RPC is synchronous (one outstanding call at a time), so a single
   command slot is race-free: the RPC handler fills g_req + g_cmd, signals, then
   waits g_done; the stream thread picks it up on its next ~8 ms tick, processes
   it (all file/SPU work happens HERE), writes the response, and signals g_done. */
static volatile int g_cmd = 0;          /* pending MUS_RPC_*; 0 = none */
static MusRpc       g_req;              /* request copied out of the RPC buffer */
static int g_sem_done = -1;            /* RPC handler waits on this for consume-ack */

/* ---- RPC server ---- */
static SifRpcServerData_t g_server;
static SifRpcDataQueue_t  g_queue;
static MusRpc             g_rpc_buf __attribute__((aligned(64)));

/* ===================== stream-thread internals ===================== */

/* Read exactly n bytes, looping at EOF when g_loop. Returns bytes read. */
static int read_full(unsigned char *dst, int n) {
    int total = 0;
    while (total < n) {
        int got = iomanX_read(g_fd, dst + total, n - total);
        if (got <= 0) {                                  /* EOF / error */
            if (g_loop) { iomanX_lseek(g_fd, 0, FIO_SEEK_SET); continue; }
            break;
        }
        if (got > n - total) got = n - total;            /* defensive clamp */
        total += got;
    }
    return total;
}

/* Patch SPU loop flags onto a freshly-read chunk for ring half `h`:
   half 0 -> first block 0x06 (LOOP_START|REPEAT, sets LSAX);
   half 1 -> last block  0x03 (REPEAT|LOOP_END, jumps to LSAX). */
static void patch_flags(unsigned char *buf, int h) {
    unsigned char *l = buf;
    unsigned char *r = buf + MUS_HALF_BYTES;
    if (h == 0) {
        l[1] = 0x06; r[1] = 0x06;
    } else {
        int last = MUS_HALF_BYTES - 16;
        l[last + 1] = 0x03; r[last + 1] = 0x03;
    }
}

/* DMA one channel-half (already in g_buf, flags patched) into SPU2 ring half h.
   Blocks until each transfer lands (own DMA -> safe to overwrite g_buf after). */
static void dma_half(int h) {
    unsigned char *l = g_buf;
    unsigned char *r = g_buf + MUS_HALF_BYTES;
    sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_DMA,
                    l, (u32 *)(MUS_L_RING + (u32)h * MUS_HALF_BYTES), MUS_HALF_BYTES);
    sceSdVoiceTransStatus(0, 1);
    sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_DMA,
                    r, (u32 *)(MUS_R_RING + (u32)h * MUS_HALF_BYTES), MUS_HALF_BYTES);
    sceSdVoiceTransStatus(0, 1);
}

/* Read the next file chunk and push it into ring half h. */
static int refill_half(int h) {
    int got = read_full(g_buf, MUS_HALF_BYTES * 2);
    if (got < MUS_HALF_BYTES * 2)
        memset(g_buf + got, 0, MUS_HALF_BYTES * 2 - got);
    patch_flags(g_buf, h);
    dma_half(h);
    return got;
}

static void apply_volume(void) {
    sceSdSetParam(SD_VOICE(0, MUS_VOICE_L) | SD_VPARAM_VOLL, (u16)g_vol);
    sceSdSetParam(SD_VOICE(0, MUS_VOICE_R) | SD_VPARAM_VOLR, (u16)g_vol);
}

static void stop_playback(void) {
    if (g_playing) {
        sceSdSetSwitch(0 | SD_SWITCH_KOFF, (1u << MUS_VOICE_L) | (1u << MUS_VOICE_R));
        g_playing = 0;
    }
    if (g_fd >= 0) { iomanX_close(g_fd); g_fd = -1; }
}

/* Open + prime both halves + key on. Returns 1 on success. */
static int start_playback(const char *path, int loop) {
    int primed;
    stop_playback();
    g_fd = iomanX_open(path, FIO_RDONLY);
    if (g_fd < 0) { printf("MUSIC(IOP): open '%s' failed rc=%d\n", path, g_fd); return 0; }
    g_loop = loop ? 1 : 0;
    g_paused = 0;

    primed  = refill_half(0);          /* chunk 0 -> half 0 */
    primed += refill_half(1);          /* chunk 1 -> half 1 */
    g_last_half = 0;

    /* Per-voice setup: L voice -> left out, R voice -> right out, native pitch. */
    {
        int vl = SD_VOICE(0, MUS_VOICE_L);
        int vr = SD_VOICE(0, MUS_VOICE_R);
        sceSdSetParam(vl | SD_VPARAM_ADSR1, SD_SET_ADSR1(1, 0, 0, 0xf));
        sceSdSetParam(vl | SD_VPARAM_ADSR2, SD_SET_ADSR2(1, 0x7f, 0, 0x0e));
        sceSdSetParam(vr | SD_VPARAM_ADSR1, SD_SET_ADSR1(1, 0, 0, 0xf));
        sceSdSetParam(vr | SD_VPARAM_ADSR2, SD_SET_ADSR2(1, 0x7f, 0, 0x0e));
        sceSdSetAddr(vl | SD_VADDR_SSA, MUS_L_RING);
        sceSdSetAddr(vr | SD_VADDR_SSA, MUS_R_RING);
        sceSdSetParam(vl | SD_VPARAM_PITCH, 0x1000);
        sceSdSetParam(vr | SD_VPARAM_PITCH, 0x1000);
        sceSdSetParam(vl | SD_VPARAM_VOLL, (u16)g_vol);
        sceSdSetParam(vl | SD_VPARAM_VOLR, 0);
        sceSdSetParam(vr | SD_VPARAM_VOLL, 0);
        sceSdSetParam(vr | SD_VPARAM_VOLR, (u16)g_vol);
        /* Route the music voices into the core's dry output mix — WITHOUT this a
           keyed-on voice is generated but never summed to output (= silence; the
           refill loop still reads the file, which is why the stick blinks). The
           SFX (voices 0-11) already have their VMIX bits set by the EE/ps2snd, so
           read-modify-write to OR ours in without clearing theirs. */
        {
            u32 vml = sceSdGetSwitch(0 | SD_SWITCH_VMIXL);
            u32 vmr = sceSdGetSwitch(0 | SD_SWITCH_VMIXR);
            vml |= (1u << MUS_VOICE_L) | (1u << MUS_VOICE_R);
            vmr |= (1u << MUS_VOICE_L) | (1u << MUS_VOICE_R);
            sceSdSetSwitch(0 | SD_SWITCH_VMIXL, vml);
            sceSdSetSwitch(0 | SD_SWITCH_VMIXR, vmr);
        }
        sceSdSetSwitch(0 | SD_SWITCH_KON, (1u << MUS_VOICE_L) | (1u << MUS_VOICE_R));
    }
    g_playing = 1;
    printf("MUSIC(IOP): playing '%s' (loop=%d primed=%d)\n", path, g_loop, primed);
    return 1;
}

/* Poll the play cursor; refill the half the cursor just left. */
static void stream_pump(void) {
    u32 nax = sceSdGetAddr(SD_VOICE(0, MUS_VOICE_L) | SD_VADDR_NAX);
    int cur = (nax >= MUS_L_RING + MUS_HALF_BYTES) ? 1 : 0;
    if (cur != g_last_half) {
        refill_half(g_last_half);
        g_last_half = cur;
    }
}

/* Process a consumed command (a LOCAL copy of the request — see stream_thread).
   PLAY never opens here: it only arms g_want_play, so the heavy open stays off
   the EE's RPC wait. */
static void process_cmd(int cmd, const MusRpc *req) {
    switch (cmd) {
    case MUS_RPC_PLAY:
        stop_playback();
        memcpy(g_play_path, req->path, sizeof(g_play_path));
        g_play_path[sizeof(g_play_path) - 1] = 0;
        g_play_loop = req->loop ? 1 : 0;
        g_want_play = 1;
        g_open_div  = 0;                  /* attempt the open on the next tick */
        break;
    case MUS_RPC_STOP:
        g_want_play = 0;
        stop_playback();
        break;
    case MUS_RPC_SETVOL:
        g_vol = req->vol;
        if (g_vol < 0) g_vol = 0; else if (g_vol > 0x3fff) g_vol = 0x3fff;
        if (g_playing && !g_paused) apply_volume();
        break;
    case MUS_RPC_PAUSE:
        g_paused = req->pause ? 1 : 0;
        if (g_playing) {
            u16 p = g_paused ? 0 : 0x1000;   /* pitch 0 freezes playback */
            sceSdSetParam(SD_VOICE(0, MUS_VOICE_L) | SD_VPARAM_PITCH, p);
            sceSdSetParam(SD_VOICE(0, MUS_VOICE_R) | SD_VPARAM_PITCH, p);
        }
        break;
    default:
        break;
    }
}

#define MUS_OPEN_RETRY_TICKS  250          /* ~1 s between open attempts (4 ms tick) */

static void stream_thread(void *arg) {
    (void)arg;
    for (;;) {
        DelayThread(4000);                 /* ~4 ms tick (snappy command latency) */

        if (g_cmd) {
            /* Copy the request to a local BEFORE acking: once we SignalSema the
               EE is free to issue the next command and overwrite g_req. The heavy
               work (open/prime) then happens here, AFTER the ack, so the EE's RPC
               returned within ~one tick instead of blocking on the open. */
            int   cmd = g_cmd;
            MusRpc req = g_req;
            g_cmd = 0;
            SignalSema(g_sem_done);
            process_cmd(cmd, &req);
        }

        /* Async open + self-retry (USB mount latency): try now, else wait ~1 s. */
        if (g_want_play && !g_playing) {
            if (g_open_div <= 0) {
                if (!start_playback(g_play_path, g_play_loop))
                    g_open_div = MUS_OPEN_RETRY_TICKS;
            } else {
                g_open_div--;
            }
        }

        if (g_playing && !g_paused)
            stream_pump();
    }
}

/* ===================== RPC server thread ===================== */

/* Runs on the RPC thread. Hands the command to the stream thread and waits only
   until it is CONSUMED (not until the open completes) — so the EE RPC returns
   within ~one tick, never blocking on file/SPU work (which also keeps that work
   off the RPC thread, avoiding the ps2snd-style deadlock). The EE is optimistic
   (treats playback as started); the IOP self-retries the open. */
static void *rpc_handler(int fno, void *buffer, int length) {
    MusRpc *m = (MusRpc *)buffer;
    (void)length;
    g_req = *m;                            /* copy request out (full write first) */
    g_cmd = fno;                           /* then publish the command */
    WaitSema(g_sem_done);                  /* wait until the stream thread consumes it */
    m->r_ok = 1;                           /* accepted */
    return buffer;
}

static void rpc_thread(void *arg) {
    (void)arg;
    sceSifSetRpcQueue(&g_queue, GetThreadId());
    sceSifRegisterRpc(&g_server, MUSICSTREAM_RPC_SID, rpc_handler,
                      &g_rpc_buf, NULL, NULL, &g_queue);
    printf("MUSIC(IOP): RPC server registered (sid=0x%08x)\n", MUSICSTREAM_RPC_SID);
    sceSifRpcLoop(&g_queue);
}

int _start(int argc, char *argv[]) {
    iop_thread_t th;
    iop_sema_t   sm;
    int tid;
    (void)argc; (void)argv;

    sm.attr = 0; sm.option = 0; sm.initial = 0; sm.max = 1;
    g_sem_done = CreateSema(&sm);

    /* Stream thread: lower priority so its blocking reads yield to the USB/cdvd
       driver threads. C linkage, modest stack. */
    th.attr = TH_C; th.option = 0; th.thread = stream_thread;
    th.stacksize = 0x1000; th.priority = 0x50;
    tid = CreateThread(&th);
    StartThread(tid, NULL);

    /* RPC server thread. */
    th.attr = TH_C; th.option = 0; th.thread = rpc_thread;
    th.stacksize = 0x1000; th.priority = 0x48;
    tid = CreateThread(&th);
    StartThread(tid, NULL);

    printf("MUSIC(IOP): musicstream.irx loaded\n");
    return MODULE_RESIDENT_END;
}
