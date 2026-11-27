// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthew Stanley
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
namespace recomp_audio_native {
// Batch small stereo transfers without changing producer PCM or cadence. A
// complete/large input block goes directly to the sink; a short tail is flushed
// explicitly at present, rate changes and close. There is no second queue.
template <uint32_t Frames> class StereoBatch {
  static_assert(Frames > 0);
  std::array<int16_t, Frames * 2> pcm_{};
  uint32_t pending_ = 0;
public:
  template <class Sink> void flush(Sink sink) {
    if (!pending_) return;
    sink(pcm_.data(), pending_);
    pending_ = 0;
  }
  template <class Sink> void push(const int16_t *pcm, uint32_t frames, Sink sink) {
    if (!pcm || !frames) return;
    if (frames > Frames - pending_) flush(sink);
    if (frames >= Frames) { sink(pcm, frames); return; }
    std::memcpy(pcm_.data() + 2 * pending_, pcm, 2 * frames * sizeof(int16_t));
    pending_ += frames;
    if (pending_ == Frames) flush(sink);
  }
  void reset() { pending_ = 0; }
};
}
