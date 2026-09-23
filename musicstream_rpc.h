/*
 * musicstream_rpc.h — shared EE<->IOP RPC interface for the PS2 music streamer.
 *
 * Included by BOTH the EE backend (playstation2.c) and the IOP module
 * (musicstream.c), so the SID, command numbers and the request/response struct
 * stay in lockstep. The streamer itself (custom SPU2 double-buffered ring) runs
 * entirely on the IOP; the EE only sends these commands. See PS2/IOP_AUDIO_PLAN.md.
 */
#ifndef MUSICSTREAM_RPC_H
#define MUSICSTREAM_RPC_H

/* RPC service id. Arbitrary but unique; 'SSMU' = HyperSolar MUsic. */
#define MUSICSTREAM_RPC_SID  0x53534d55

/* RPC function numbers (the `fno` passed to sceSifCallRpc / seen by the server). */
enum {
    MUS_RPC_PLAY   = 1,   /* open path, prime ring, key on (uses .path, .loop)  */
    MUS_RPC_STOP   = 2,   /* key off + close file                                */
    MUS_RPC_SETVOL = 3,   /* set music volume (uses .vol, 0..0x3fff)             */
    MUS_RPC_PAUSE  = 4,   /* pause/resume (uses .pause)                          */
    MUS_RPC_SD_BATCH = 5, /* apply queued EE SFX voice writes in order (MusSdBatch) */
    MUS_RPC_COMMAND_QUEUE = 6, /* own one music command without waiting for file I/O */
};

/* One EE frame's SFX register writes, applied in order by the RPC thread with
   the same libsd calls the EE would otherwise send one RPC each. The EE only
   uses it under PS2_AUDIO_SD_BATCH; the handler is inert otherwise. */
enum { MUS_SD_SET_PARAM = 1, MUS_SD_SET_SWITCH = 2, MUS_SD_SET_ADDR = 3 };
#define MUS_SD_BATCH_MAX 126
typedef struct {
    unsigned short kind;    /* MUS_SD_* */
    unsigned short entry;   /* libsd entry (voice/core | parameter) */
    unsigned int   value;
} MusSdOp;
typedef struct {
    int     count;          /* 1..MUS_SD_BATCH_MAX */
    int     pad[3];
    MusSdOp op[MUS_SD_BATCH_MAX];
} MusSdBatch;               /* 16 + 126 * 8 = 1024 bytes, SIF-aligned size */

/* 16-byte reply. The IOP validates the whole packet before applying any op:
   REJECTED means nothing was applied; OK means every op was applied. */
enum { MUS_SD_BATCH_NONE = 0, MUS_SD_BATCH_OK = 1, MUS_SD_BATCH_REJECTED = 2 };
typedef struct {
    int status;             /* MUS_SD_BATCH_* (EE presets NONE before sending) */
    int applied;            /* ops applied: count on OK, 0 on REJECTED */
    int pad[2];
} MusSdBatchReply;

/* Single request/response buffer (DMA'd both ways). 256 + 32 = 288 bytes, a
   multiple of 16 (SIF DMA alignment). The EE fills the request fields; the IOP
   fills the r_* response fields in place. */
typedef struct {
    int  loop;          /* PLAY:  1 = loop the track at EOF                       */
    int  vol;           /* SETVOL: 0..0x3fff                                      */
    int  pause;         /* PAUSE: 1 = pause, 0 = resume                           */
    int  pad0;
    char path[256];     /* PLAY:  device path, e.g. "mass0:/hypersolar.adpcm"     */
    int  r_ok;          /* legacy response: 1 = consumed, not playback ready     */
    int  r_fd;          /* response diag: file descriptor (or negative errno)     */
    int  r_primed;      /* response diag: bytes primed into the ring              */
    int  r_pad;
} MusRpc;

/* Queue admission is separate from legacy consume-ack commands. The request
   is copied into IOP-owned storage before OK; FULL applies nothing. */
#define MUS_COMMAND_QUEUE_MAX 16u
typedef struct {
    int command;            /* MUS_RPC_PLAY/STOP/SETVOL/PAUSE */
    unsigned int sequence;
    int pad[2];
    MusRpc request;
} MusQueuedCommand;
enum { MUS_QUEUE_NONE = 0, MUS_QUEUE_OK = 1, MUS_QUEUE_FULL = 2,
       MUS_QUEUE_REJECTED = 3 };
#define MUS_QUEUE_REPLY_MAGIC 0x4d515231u
typedef struct {
    unsigned int magic;
    unsigned int sequence;
    int status;
    unsigned int depth;
} MusQueueReply;
typedef char MusQueuedCommandSizeCheck[sizeof(MusQueuedCommand) == 304 ? 1 : -1];
typedef char MusQueueReplySizeCheck[sizeof(MusQueueReply) == 16 ? 1 : -1];

#endif /* MUSICSTREAM_RPC_H */
