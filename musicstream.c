/*
 * musicstream.irx — custom IOP music streamer for HyperSolar (PS2).
 *
 * Replaces the EE-driven streamer: the EE only sends play/stop/vol/pause over
 * SIF RPC; everything else — reading the .adpcm and feeding SPU2 — runs here on
 * the IOP. The EE handles control admission, with no music decode/refill work.
 *
 * Design (see PS2/IOP_AUDIO_PLAN.md, PS2/LESSONS.md):
 *   - Stereo = two SPU2 voices (22 L, 23 R), each a 2-half ring in SPU2 RAM.
 *     The .adpcm is chunk-interleaved [L:CHUNK][R:CHUNK]; one CHUNK == one half.
 *   - A dedicated stream thread polls the play cursor (NAX) every ~4 ms and,
 *     when the cursor crosses into a new half, refills the half it just left
 *     with the next file chunk. The ring self-loops via the SPU block flags
 *     (0x06 start / 0x03 end), so playback is gapless; the file rewinds at EOF.
 *   - THE RULE that beats ps2snd: the blocking file read + SPU DMA happen ONLY
 *     on the stream thread, never inside an IRQ or the RPC handler. The RPC
 *     queue handler owns a command copy before replying. The legacy handler
 *     waits for consumption and can stall behind the worker's previous I/O.
 *   - Uses the SAME libsd (sceSd*) the SFX use (voices 0-14). We never call
 *     sceSdInit — the EE already initialized SPU2 via ps2snd. Music owns
 *     voices 22/23; the optional SFX batch handles EE-authored voice writes.
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
#include <intrman.h>

#include "musicstream_rpc.h"

IRX_ID("hypersolar_music", 1, 0);

/* ---- Ring geometry (MUST match Makefile PS2_MUSIC_CHUNK and the EE side) ---- */
#define MUS_CHUNK_BLOCKS  1024
#define MUS_HALF_BYTES    (MUS_CHUNK_BLOCKS * 16)        /* 16 KB per channel-half */
#define MUS_RING_BYTES    (MUS_HALF_BYTES * 2)           /* 32 KB per channel ring */
#define MUS_L_RING        0x1F0000u
#define MUS_R_RING        (MUS_L_RING + MUS_RING_BYTES)  /* 0x1F8000 */
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

/* A consumed PLAY only requests playback. The stream thread opens/primes below
   and retries USB mount failures. Legacy consumption can wait behind prior
   I/O; queue admission does not wait for this worker. */
static int  g_want_play  = 0;            /* should be playing g_play_path        */
static int  g_play_loop  = 1;
static int  g_open_div   = 0;            /* ticks until the next open attempt     */
static char g_play_path[256];
static unsigned char g_buf[MUS_HALF_BYTES * 2] __attribute__((aligned(64)));  /* one [L][R] chunk */

/* ---- EE command handshake (RPC thread -> stream thread) ----
   The EE RPC is synchronous (one outstanding call at a time), so a single
   command slot is race-free: the RPC handler fills g_req + g_cmd, signals, then
   waits g_done. The stream thread copies it on a later ~4 ms tick and signals
   g_done BEFORE processing. A previous file read can delay that consumption. */
static volatile int g_cmd = 0;          /* pending MUS_RPC_*; 0 = none */
static MusRpc       g_req;              /* request copied out of the RPC buffer */
static int g_sem_done = -1;            /* RPC handler waits on this for consume-ack */

/* Queue-mode commands are owned here before the RPC replies. Only these
   bounded copies/cursors are protected; file and SPU work runs after pop. */
static MusQueuedCommand g_commands[MUS_COMMAND_QUEUE_MAX];
static volatile unsigned int g_command_head, g_command_tail;
static MusQueueReply g_command_reply __attribute__((aligned(64)));

static int command_pop(MusQueuedCommand *command) {
    int state;
    if (g_command_head == g_command_tail) return 0;
    CpuSuspendIntr(&state);
    if (g_command_head == g_command_tail) {
        CpuResumeIntr(state);
        return 0;
    }
    *command = g_commands[g_command_tail % MUS_COMMAND_QUEUE_MAX];
    ++g_command_tail;
    CpuResumeIntr(state);
    return 1;
}

/* ---- RPC server ---- */
static SifRpcServerData_t g_server;
static SifRpcDataQueue_t  g_queue;
/* Receive buffer sized for the largest request (the SFX batch). */
static union {
    MusRpc     mus;
    MusSdBatch sd;
    MusQueuedCommand command;
} g_rpc_buf __attribute__((aligned(64)));
static MusSdBatchReply g_sd_reply __attribute__((aligned(64)));

/* ---- Serialized SPU2 register writes ----
   SPU2 Overview Manual (1.1; KON/KOF notes pp.53-54): a register must not be
   written twice within 1 Ts (2 Ts for KON/KOF), and KON/KOF of one voice must
   be 2 Ts apart (1 Ts = 1/48000 s). Every SPU register write this module
   makes, from the RPC thread (SFX batch) or the stream thread (music voices),
   goes through sd_write under one lock. It uses a shared log of recent writes
   and a per-core key clock, so the spacing holds across batches, across
   threads and across different voices sharing KON/KOF/VMIX. The spin polls the
   system clock with interrupts enabled; it never assumes elapsed time.
   The interval is 2 Ts (41.7 us) rounded up to 43 us, plus one clock tick for
   quantization of the two timestamps, for every register. */
#define MUS_SD_SPACING_USEC 43u
#define MUS_SD_LOG 32u                           /* power of two */
static int g_sd_lock = -1;
static u64 g_sd_spacing_clocks;
static u64 g_sd_keyed_at[2];                     /* last KON/KOF write per core */
static struct {
    u64 at;
    unsigned short kind, entry;
} g_sd_log[MUS_SD_LOG];                          /* newest at g_sd_log_head - 1 */
static unsigned g_sd_log_head;

static u64 sd_clock(void) {
    iop_sys_clock_t c;
    GetSystemTime(&c);
    return ((u64)c.hi << 32) | c.lo;
}

static void sd_spacing_wait(u64 since) {
    if (!since) return;
    while (sd_clock() - since <= g_sd_spacing_clocks) {
    }
}

/* A failed WaitSema is never ownership: the caller must not write. */
static int sd_lock(void) { return WaitSema(g_sd_lock) >= 0; }
static void sd_unlock(void) { SignalSema(g_sd_lock); }

/* Caller holds the lock. */
static void sd_write_locked(unsigned kind, u16 entry, u32 value) {
    unsigned k;
    const int is_key = kind == MUS_SD_SET_SWITCH &&
        ((entry & 0xff00u) == SD_SWITCH_KON ||
         (entry & 0xff00u) == SD_SWITCH_KOFF);
    if (is_key) sd_spacing_wait(g_sd_keyed_at[entry & 1u]);
    /* The slot this write will reuse may only be evicted once its entry has
       expired, so the log always holds every register written within the
       interval, however fast the writes arrive. */
    sd_spacing_wait(g_sd_log[g_sd_log_head & (MUS_SD_LOG - 1u)].at);
    for (k = 1; k <= MUS_SD_LOG; k++) {          /* newest first */
        const unsigned idx = (g_sd_log_head - k) & (MUS_SD_LOG - 1u);
        if (!g_sd_log[idx].at ||
            sd_clock() - g_sd_log[idx].at > g_sd_spacing_clocks) break;
        if (g_sd_log[idx].kind == kind && g_sd_log[idx].entry == entry) {
            sd_spacing_wait(g_sd_log[idx].at);
            break;
        }
    }
    if (kind == MUS_SD_SET_PARAM)
        sceSdSetParam(entry, (u16)value);
    else if (kind == MUS_SD_SET_SWITCH)
        sceSdSetSwitch(entry, value);
    else
        sceSdSetAddr(entry, value);
    {
        const u64 now = sd_clock();
        const unsigned idx = g_sd_log_head & (MUS_SD_LOG - 1u);
        g_sd_log[idx].at = now;
        g_sd_log[idx].kind = (unsigned short)kind;
        g_sd_log[idx].entry = entry;
        g_sd_log_head++;
        if (is_key) g_sd_keyed_at[entry & 1u] = now;
    }
}

/* Latched once: an ownership failure is impossible after a successful
   startup, so any dropped write is reported rather than hidden. */
static int g_sd_lock_failed;

static void sd_lock_failure(u16 entry) {
    if (g_sd_lock_failed) return;
    g_sd_lock_failed = 1;
    printf("MUSIC(IOP): SPU write lock failed; write %04x dropped, "
           "music state unreliable\n", (unsigned)entry);
}

static void sd_write(unsigned kind, u16 entry, u32 value) {
    if (!sd_lock()) {                /* no unserialized write */
        sd_lock_failure(entry);
        return;
    }
    sd_write_locked(kind, entry, value);
    sd_unlock();
}

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
    sd_write(MUS_SD_SET_PARAM, SD_VOICE(0, MUS_VOICE_L) | SD_VPARAM_VOLL, (u16)g_vol);
    sd_write(MUS_SD_SET_PARAM, SD_VOICE(0, MUS_VOICE_R) | SD_VPARAM_VOLR, (u16)g_vol);
}

static void stop_playback(void) {
    if (g_playing) {
        sd_write(MUS_SD_SET_SWITCH, 0 | SD_SWITCH_KOFF,
                 (1u << MUS_VOICE_L) | (1u << MUS_VOICE_R));
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
        sd_write(MUS_SD_SET_PARAM, vl | SD_VPARAM_ADSR1, SD_SET_ADSR1(1, 0, 0, 0xf));
        sd_write(MUS_SD_SET_PARAM, vl | SD_VPARAM_ADSR2, SD_SET_ADSR2(1, 0x7f, 0, 0x0e));
        sd_write(MUS_SD_SET_PARAM, vr | SD_VPARAM_ADSR1, SD_SET_ADSR1(1, 0, 0, 0xf));
        sd_write(MUS_SD_SET_PARAM, vr | SD_VPARAM_ADSR2, SD_SET_ADSR2(1, 0x7f, 0, 0x0e));
        sd_write(MUS_SD_SET_ADDR, vl | SD_VADDR_SSA, MUS_L_RING);
        sd_write(MUS_SD_SET_ADDR, vr | SD_VADDR_SSA, MUS_R_RING);
        sd_write(MUS_SD_SET_PARAM, vl | SD_VPARAM_PITCH, 0x1000);
        sd_write(MUS_SD_SET_PARAM, vr | SD_VPARAM_PITCH, 0x1000);
        sd_write(MUS_SD_SET_PARAM, vl | SD_VPARAM_VOLL, (u16)g_vol);
        sd_write(MUS_SD_SET_PARAM, vl | SD_VPARAM_VOLR, 0);
        sd_write(MUS_SD_SET_PARAM, vr | SD_VPARAM_VOLL, 0);
        sd_write(MUS_SD_SET_PARAM, vr | SD_VPARAM_VOLR, (u16)g_vol);
        /* Route the music voices into the core's dry output mix — WITHOUT this a
           keyed-on voice is generated but never summed to output (= silence; the
           refill loop still reads the file, which is why the stick blinks). The
           SFX (voices 0-11) already have their VMIX bits set by the EE/ps2snd, so
           read-modify-write to OR ours in without clearing theirs. */
        {
            /* The read-modify-write holds the lock, so no batch VMIX write
               can land between the read and our write. */
            if (!sd_lock()) {
                sd_lock_failure(0 | SD_SWITCH_VMIXL);
            } else {
                u32 vml = sceSdGetSwitch(0 | SD_SWITCH_VMIXL);
                u32 vmr = sceSdGetSwitch(0 | SD_SWITCH_VMIXR);
                vml |= (1u << MUS_VOICE_L) | (1u << MUS_VOICE_R);
                vmr |= (1u << MUS_VOICE_L) | (1u << MUS_VOICE_R);
                sd_write_locked(MUS_SD_SET_SWITCH, 0 | SD_SWITCH_VMIXL, vml);
                sd_write_locked(MUS_SD_SET_SWITCH, 0 | SD_SWITCH_VMIXR, vmr);
                sd_unlock();
            }
        }
        sd_write(MUS_SD_SET_SWITCH, 0 | SD_SWITCH_KON,
                 (1u << MUS_VOICE_L) | (1u << MUS_VOICE_R));
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
            sd_write(MUS_SD_SET_PARAM, SD_VOICE(0, MUS_VOICE_L) | SD_VPARAM_PITCH, p);
            sd_write(MUS_SD_SET_PARAM, SD_VOICE(0, MUS_VOICE_R) | SD_VPARAM_PITCH, p);
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
               need not wait for this command's open. Previous I/O can still
               delay reaching this consume point in the legacy path. */
            int   cmd = g_cmd;
            MusRpc req = g_req;
            g_cmd = 0;
            SignalSema(g_sem_done);
            process_cmd(cmd, &req);
        }

        /* Match the old one-command-per-tick processing order: a PLAY still
           reaches open/prime below before the next queued command is taken. */
        {
            MusQueuedCommand command;
            if (command_pop(&command))
                process_cmd(command.command, &command.request);
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

/* Queue admission replies after an owned copy. Legacy control RPCs wait for
   stream consumption, which may be behind a previous file read/refill. Neither
   acknowledgement establishes command application or successful file open. */
static void *rpc_handler(int fno, void *buffer, int length) {
    if (fno == MUS_RPC_COMMAND_QUEUE) {
        const MusQueuedCommand *command = (const MusQueuedCommand *)buffer;
        int state;
        int valid = length == (int)sizeof(*command);
        if (valid)
            valid = command->command >= MUS_RPC_PLAY &&
                    command->command <= MUS_RPC_PAUSE;
        g_command_reply.magic = MUS_QUEUE_REPLY_MAGIC;
        g_command_reply.sequence = length == (int)sizeof(*command) ? command->sequence : 0;
        g_command_reply.status = MUS_QUEUE_REJECTED;
        CpuSuspendIntr(&state);
        if (valid) {
            if (g_command_head - g_command_tail == MUS_COMMAND_QUEUE_MAX) {
                g_command_reply.status = MUS_QUEUE_FULL;
            } else {
                g_commands[g_command_head % MUS_COMMAND_QUEUE_MAX] = *command;
                ++g_command_head;          /* publish only after the owned copy */
                g_command_reply.status = MUS_QUEUE_OK;
            }
        }
        g_command_reply.depth = g_command_head - g_command_tail;
        CpuResumeIntr(state);
        return &g_command_reply;
    }
    if (fno == MUS_RPC_SD_BATCH) {
        /* SFX register writes only: apply in EE order right here, like
           ps2snd's own server does per call. No file/transfer work and no
           stream-thread handshake, so the EE waits for one round trip.
           Validate the whole packet first: all ops or none. */
        const MusSdBatch *b = (const MusSdBatch *)buffer;
        const int n = b->count;
        int i;
        int ok = n >= 1 && n <= MUS_SD_BATCH_MAX &&
                 length >= 16 + n * (int)sizeof(MusSdOp);
        for (i = 0; ok && i < n; i++) {
            const unsigned short kind = b->op[i].kind;
            ok = kind == MUS_SD_SET_PARAM || kind == MUS_SD_SET_SWITCH ||
                 kind == MUS_SD_SET_ADDR;
        }
        g_sd_reply.status = ok ? MUS_SD_BATCH_OK : MUS_SD_BATCH_REJECTED;
        g_sd_reply.applied = 0;
        /* One lock for the batch keeps its writes contiguous; sd_write's
           clock/log enforce the SPU2 spacing between any two of them. A lock
           failure applies nothing and reports REJECTED, so the EE replays. */
        if (ok && !sd_lock()) {
            sd_lock_failure(b->op[0].entry);
            ok = 0;
            g_sd_reply.status = MUS_SD_BATCH_REJECTED;
        }
        if (ok) {
            for (i = 0; i < n; i++) {
                const MusSdOp *op = &b->op[i];
                sd_write_locked(op->kind, op->entry, op->value);
                g_sd_reply.applied = i + 1;
            }
            sd_unlock();
        }
        return &g_sd_reply;
    }
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
    /* The SPU write lock is a synchronization boundary: without it no RPC
       server may be advertised. Created taken (initial 0), then released. */
    g_sd_lock = CreateSema(&sm);
    if (g_sd_lock < 0 || SignalSema(g_sd_lock) < 0) {
        printf("MUSIC(IOP): SPU write lock unavailable (%d); not loading\n",
               g_sd_lock);
        if (g_sd_lock >= 0) DeleteSema(g_sd_lock);
        if (g_sem_done >= 0) DeleteSema(g_sem_done);
        return MODULE_NO_RESIDENT_END;
    }
    {
        iop_sys_clock_t spacing;
        USec2SysClock(MUS_SD_SPACING_USEC, &spacing);
        g_sd_spacing_clocks = (((u64)spacing.hi << 32) | spacing.lo) + 1u;
    }

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
