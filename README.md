# musicstream.irx

A small, self-contained **PlayStation 2 IOP module** that streams stereo music to
the SPU2 with **no EE decoding or refill work**. The EE sends
`play/stop/vol/pause` over SIF RPC; the IOP reads files and feeds the SPU2.
The optional queued interface also needs a bounded EE retry pump for commands
not yet admitted by the IOP.

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
  open is async and self-retries (for removable-media mount latency). Legacy
  commands wait for the stream thread to consume their shared slot, so they
  can still block the EE behind an in-progress read/refill. The queued command
  interface below removes that consume wait.
- Uses the same `libsd` (`sceSd*`) the rest of your audio uses. The music
  worker owns voices 22/23 and never calls `sceSdInit`. An optional SFX batch
  RPC applies the EE's supplied register writes on the RPC thread. Both
  writers share the SPU2 register/key timing guard.

## Audio format

Raw **PS-ADPCM**, 16-byte blocks, stereo, chunk-interleaved. `MUS_CHUNK_BLOCKS`
(default 1024 = a 16 KB half per channel, ~0.6 s at 48 kHz) must match how the
file was encoded. No VAG header. 48 kHz native (pitch `0x1000`).

Changing the chunk size requires regenerating and replacing external music
files as well as rebuilding the module. Raw ADPCM has no header identifying
the interleave. In HyperSolar's September 8, 2026 hardware test, old 2048-block
USB files played through the 1024-block reader sent successive sections of
one channel to opposite outputs, producing a 0.597-second offset and doubled
vocals. Replacing the files fixed playback without a runtime audio change.
Check the deployed data before assuming that music was started twice.

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

For frame-sensitive callers, `MUS_RPC_COMMAND_QUEUE` accepts a
`MusQueuedCommand` and returns a `MusQueueReply`. Validate its magic and echoed
sequence as well as status: older modules do not implement this opcode.
`MUS_QUEUE_OK` means the IOP owns a copy, not that playback has started.
`MUS_QUEUE_FULL` means nothing was admitted; retain the command and retry in
order. `MUS_QUEUE_REJECTED` is a contract failure, not permission to replay an
unknown outcome. The worker consumes the bounded FIFO in order, outside the
short publication critical section, before its existing open/refill step.
Do not mix queued and legacy music controls while either has pending work.
SFX batches remain a distinct command with their existing application reply.

HyperSolar gates its EE queue with `PS2_AUDIO_MUSIC_QUEUE` and `PS2_TEMP`.
It retains pending commands, changes optimistic state only after local
admission, and reports exceptional queue exhaustion rather than silently
discarding controls. This avoids waiting for media work; it does not make
the SDK's synchronous RPC transport immune to a dead server.

## Build

```
make PLATFORM=ps2 PS2/lib/musicstream/musicstream.irx
```

Run this from HyperSolar's repository root, through its owned Makefile. A
normal `make ps2` or `make ps2-hw CITY_BENCH=1` also rebuilds and embeds the
module when its source/interface changes. Do not bypass that build entrypoint
when working inside HyperSolar.

Then embed/`SifExecModuleBuffer` it from your EE program, or `IopLoadModule` it
from a device.

## License

Provided as-is for the PS2 homebrew community.
