# Optional Aurora output profile

This separately instantiated backend retains the qualified read-only source
inputs `aurora_audio.cpp` and `audio_stretch.hpp` from RecompCore commit
`338a3dbcdd384142f0fe40db44e0cd1d1b0c61e4`, authored by Chris Sotraidis under
GPL-3.0-or-later. `COPYING` retains the donor overview notice and
`GPL-3.0.txt` retains its GPL version 3 license text. `adapt.cmake` contains
guarded fragments of that GPL source; its authored adaptation glue also retains
the owner's MIT grant. Neither the GPL backend nor its dependencies are part of
the portable `recomp-audio::core` or `recomp-audio::session` targets.

A caller with SDL3 and the qualified source tree explicitly includes
`provider.cmake`, calls `recomp_audio_add_aurora_backend(GXRuntime [ROLLBACK])`,
and links `recomp-audio::aurora-gpl`. Line-ending-normalized source hashes and guarded replacements
reject a different backend rather than silently substituting one. The linked
profile includes GPL material and must preserve the applicable GPL terms and
corresponding source; the owner MIT grant does not relicense donor contributions.

The owner-authored native session retains 64-frame batching of small stereo
transfers, direct submission of larger blocks, tail flush, 32/48 kHz selection,
queue/prebuffer policy, paused-queue retention, recovery, discontinuity/replay
controls, capture and demand observations. Time stretching stays disabled in
this qualified profile. The caller retains console platform registration and
presentation deadlines. A single process profile matches the upstream globals.
