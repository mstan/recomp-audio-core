# recomp-audio-core

Shared portable C99 host audio clock bridge extracted from qualified NES and
Genesis implementations. It consumes interleaved S16 native PCM and produces
interleaved S16 device-rate PCM through a persistent polyphase resampler, bounded
ring, normalized-fill controller, startup gate and stall concealment. No guest
instruction execution, console synthesis or mixer is included.

## Consumer contract

Link `recomp-audio::core` and include `recomp_audio.h`. Call
`rab_config_defaults`, set channels/source_rate/host_rate and choose
`delivery_profile` before `recomp_audio_create`:

- `RECOMP_AUDIO_GENESIS` (0): bounded coarse/refined stall-loop search and queue
  refill before recovery, matching the pinned Genesis donor.
- `RECOMP_AUDIO_LEGACY_NES` (1): exhaustive stall-loop search and immediate
  recovery, matching the pinned NES donor.

Profiles stay fixed for an instance. They describe existing device delivery
behavior, not guest execution backends. There is no silent runtime fallback.
Push source blocks with `recomp_audio_push`; the device callback requests its
exact output frame count with `recomp_audio_pull`. Observe fill and counters
with `recomp_audio_fill_ms` and `recomp_audio_stats`. Reset when pausing/loading
requires discarded history; destroy only after stopping the callback.

**The caller must serialize push, pull, reset, observation and destruction.**
The inherited queue is not lock-free: overflow advances the consumer cursor.
Use the existing device lock or an equivalent exclusion primitive. The API adds
no per-sample callbacks, worker threads, SDL dependency or device/window.
Each instance owns its buffers and controller state. The host owns its source
clock, device lifecycle, device locking, pause/resume policy and capture sink.
Capture the producer PCM or returned output blocks at the same observation
point used previously; this extraction does not move capture across mixing,
gain or resampling.

`recomp_audio_drc.h` also exposes the donor-compatible direct `rab_*` block API
for source consumers. `rab_reset` reuses the allocated filter/ring;
`rab_try_push_prepared` copies normalized F32 blocks only when it can prove
scalar overflow handling unnecessary, and reports refusal without mutation.
On refusal use the original S16 block with `rab_push`. Config layout adds a
profile field; initialize with defaults instead of positional initializers.
Raw serialized `rab_bridge` structures are not a stable file format.

## Build and checks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --timeout 0
```

The tests compile frozen, unmodified donor implementations in separate
translation units and compare every output sample, ring hash, controller and
concealment state, cursor and diagnostic counter. Scenarios cover two profiles,
mono/stereo, five source rates, two device rates, startup, sustained irregular
blocks, starvation/recovery, overflow, wrapping, reset and nonfinite dimensions.
Additional checks cover prepared-F32 refusal above exact-double counts and C++
linkage of the opaque C interface. The core uses portable scalar math; no
assembly or platform-specific intrinsics are required.

Component checks do not establish game integration, live device latency,
speaker quality, netplay compatibility or a runtime performance improvement.
The original system consumers and their production pins are unchanged.

## Provenance and licensing

Exact donor revisions and byte hashes are in `PROVENANCE.json`. Frozen headers
under `tests/oracles` are test-only originals, not production implementations.
The shared implementation retains the pinned Genesis code except for the
explicit legacy-NES policy, config validation and extraction wrapper. No
previously rejected synthesis-to-device pipeline replacement is included.

The code is covered by the included PolyForm Noncommercial License 1.0.0.
Donor permission grants are retained in `licenses/`. No game images, assets,
firmware, captures or generated guest source are distributed here.
