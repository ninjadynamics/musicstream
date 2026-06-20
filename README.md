# musicstream.irx

A small, self-contained **PlayStation 2 IOP module** that streams stereo music to
the SPU2 with **zero EE per-frame work** — the EE just sends `play/stop/vol/pause`
over SIF RPC and the IOP does all the file reading and SPU feeding itself.

Built for [HyperSolar](https://github.com/ninjadynamics) but engine-agnostic: any
EE program can load it, bind its RPC, and stream music.

## Why this exists

ps2sdk's `ps2snd` streaming (`sndStreamOpen`) **deadlocks the IOP** when it opens
a file to play (it does the device mount/open inside its own blocking RPC, which
collides with the USB/cdvd driver threads). This module avoids that entirely by
owning the stream loop on a dedicated IOP thread and never doing file I/O inside
an IRQ or the RPC handler.

## How it works

- **Stereo = two SPU2 voices** (22 L, 23 R), each a **2-half ring** in SPU2 RAM.
  The input is chunk-interleaved `[L:CHUNK][R:CHUNK]…`; one CHUNK fills one half.
- A **stream thread** polls the play cursor (`sceSdGetAddr … NAX`) every ~4 ms and
  refills the half the cursor just left with the next file chunk via
  `sceSdVoiceTrans`. The ring self-loops on the SPU block flags (`0x06` start /
  `0x03` end), so playback is gapless; the file rewinds at EOF when looping.
- **The rule that beats ps2snd:** the blocking file read + SPU DMA happen ONLY on
  the stream thread. The RPC handler just hands off a command and returns; the
  open is async and self-retries (for removable-media mount latency), so the EE
  never blocks.
- Uses the same `libsd` (`sceSd*`) the rest of your audio uses — it drives only
  its two voices and never calls `sceSdInit`, so it coexists with EE-side SFX.

## Audio format

Raw **PS-ADPCM**, 16-byte blocks, stereo, chunk-interleaved. `MUS_CHUNK_BLOCKS`
(default 2048 = a 32 KB half per channel, ~1.2 s at 48 kHz) must match how the
file was encoded. No VAG header. 48 kHz native (pitch `0x1000`).

## RPC interface

See [`musicstream_rpc.h`](musicstream_rpc.h) — the shared EE/IOP header (SID,
command numbers, request/response struct). EE side:

```c
SifRpcClientData_t cd;
MusRpc rpc __attribute__((aligned(64)));
sceSifBindRpc(&cd, MUSICSTREAM_RPC_SID, 0);          /* retry until cd.server */
/* play: */
snprintf(rpc.path, sizeof(rpc.path), "mass0:/track.adpcm");
rpc.loop = 1;
sceSifCallRpc(&cd, MUS_RPC_PLAY, 0, &rpc, sizeof(rpc), &rpc, sizeof(rpc), 0, 0);
```

The device is whatever the IOP can `open()` through iomanX (e.g. `mass0:` via the
BDM/USB stack). Load the device drivers before binding.

## Build

```
make            # -> musicstream.irx  (needs the ps2dev IOP toolchain on PATH)
```

Then embed/`SifExecModuleBuffer` it from your EE program, or `IopLoadModule` it
from a device.

## License

Provided as-is for the PS2 homebrew community.
