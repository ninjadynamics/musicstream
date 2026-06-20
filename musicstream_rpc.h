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

/* RPC service id. Arbitrary but unique; 'SSMU' = SuperSolar MUsic. */
#define MUSICSTREAM_RPC_SID  0x53534d55

/* RPC function numbers (the `fno` passed to sceSifCallRpc / seen by the server). */
enum {
    MUS_RPC_PLAY   = 1,   /* open path, prime ring, key on (uses .path, .loop)  */
    MUS_RPC_STOP   = 2,   /* key off + close file                                */
    MUS_RPC_SETVOL = 3,   /* set music volume (uses .vol, 0..0x3fff)             */
    MUS_RPC_PAUSE  = 4,   /* pause/resume (uses .pause)                          */
};

/* Single request/response buffer (DMA'd both ways). 256 + 32 = 288 bytes, a
   multiple of 16 (SIF DMA alignment). The EE fills the request fields; the IOP
   fills the r_* response fields in place. */
typedef struct {
    int  loop;          /* PLAY:  1 = loop the track at EOF                       */
    int  vol;           /* SETVOL: 0..0x3fff                                      */
    int  pause;         /* PAUSE: 1 = pause, 0 = resume                           */
    int  pad0;
    char path[256];     /* PLAY:  device path, e.g. "mass0:/supersolar.adpcm"     */
    int  r_ok;          /* response: 1 = playing (opened + primed + keyed on)     */
    int  r_fd;          /* response diag: file descriptor (or negative errno)     */
    int  r_primed;      /* response diag: bytes primed into the ring              */
    int  r_pad;
} MusRpc;

#endif /* MUSICSTREAM_RPC_H */
