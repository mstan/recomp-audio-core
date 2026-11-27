// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthew Stanley
#include "recomp_audio_session.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace {
constexpr size_t packets = RECOMP_AUDIO_PACKET_COUNT,
                 max_samples = RECOMP_AUDIO_PACKET_SAMPLES;
constexpr uint64_t end_budget_ns = 64000000, prime_ms = 20;
uint64_t host_clock() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
uint64_t saturated_add(uint64_t a, uint64_t b) {
  return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
bool byte_count_valid(int64_t bytes) {
  return bytes >= 0 && uint64_t(bytes) <= UINT32_MAX;
}
} // namespace
extern "C" uint64_t recomp_audio_session_clock_ns(void) { return host_clock(); }
struct RecompAudioPacketSession {
  RecompAudioPacket slots[packets]{};
  std::unique_ptr<int16_t[]> storage;
  std::atomic_size_t write{0}, read{0};
  size_t producer_write = 0, consumer_read = 0;
  std::atomic_uint32_t queued_frames{0};
  std::atomic_uint64_t submit_ns{0};
  uint32_t rate = 0, channels = 0;
  bool paced = false, priming = false, reported_overflow = false;
  uint64_t next_sequence = 0;
  RecompAudioPacketObserver observer{};
  uint64_t now() const {
    return observer.clock_ns ? observer.clock_ns(observer.user) : host_clock();
  }
  void discard(uint64_t seq, int reason, bool first = false) {
    if (observer.discarded)
      observer.discarded(observer.user, seq, reason, int(first));
  }
  void advance() {
    ++consumer_read;
    read.store(consumer_read, std::memory_order_release);
  }
  void prune(size_t published, uint64_t now_ns, uint32_t device_frames) {
    if (!paced)
      return;
    while (published - consumer_read > 1) {
      const auto &p = slots[consumer_read % packets];
      const uint64_t frames = uint64_t(device_frames) + p.samples / channels;
      const uint64_t age = now_ns > p.produced_ns ? now_ns - p.produced_ns : 0;
      if (saturated_add(age, frames * 1000000000ull / rate) <= end_budget_ns)
        break;
      const auto seq = p.sequence;
      advance();
      discard(seq, RECOMP_AUDIO_PACED_BACKLOG);
    }
  }
};
extern "C" RecompAudioPacketSession *
recomp_audio_packet_create(uint32_t rate, uint32_t channels, int paced,
                           const RecompAudioPacketObserver *observer) {
  if (!rate || !channels || channels > 2)
    return nullptr;
  std::unique_ptr<RecompAudioPacketSession> q(new (std::nothrow)
                                                  RecompAudioPacketSession);
  if (!q)
    return nullptr;
  q->storage.reset(new (std::nothrow) int16_t[packets * max_samples]);
  if (!q->storage)
    return nullptr;
  q->rate = rate;
  q->channels = channels;
  q->paced = paced != 0;
  q->priming = q->paced;
  if (observer)
    q->observer = *observer;
  for (size_t i = 0; i < packets; ++i)
    q->slots[i].pcm = q->storage.get() + i * max_samples;
  return q.release();
}
extern "C" void recomp_audio_packet_destroy(RecompAudioPacketSession *q) {
  delete q;
}
extern "C" int recomp_audio_packet_submit(RecompAudioPacketSession *q,
                                          const int16_t *pcm, size_t count,
                                          uint64_t field, uint64_t at) {
  if (!q || count > max_samples || count % q->channels || (count && !pcm))
    return -1;
  if (!count)
    return 0;
  const uint64_t seq =
      q->observer.produced
          ? q->observer.produced(q->observer.user, count, field, at)
          : ++q->next_sequence;
  const bool full =
      q->producer_write - q->read.load(std::memory_order_acquire) == packets;
  if (full) {
    const bool first = q->paced && !q->reported_overflow;
    q->reported_overflow |= q->paced;
    q->discard(seq,
               q->paced ? RECOMP_AUDIO_PACED_OVERFLOW
                        : RECOMP_AUDIO_UNPACED_OVERFLOW,
               first);
  } else {
    auto &p = q->slots[q->producer_write % packets];
    std::memcpy(const_cast<int16_t *>(p.pcm), pcm, count * sizeof(int16_t));
    p.samples = count;
    p.sequence = seq;
    p.produced_ns = at;
    ++q->producer_write;
    q->write.store(q->producer_write, std::memory_order_release);
  }
  if (q->observer.send_returned)
    q->observer.send_returned(q->observer.user, seq);
  return full ? 1 : 0;
}
extern "C" int recomp_audio_packet_peek(RecompAudioPacketSession *q,
                                        RecompAudioPacket *out) {
  if (!q || !out ||
      q->consumer_read == q->write.load(std::memory_order_acquire))
    return 0;
  *out = q->slots[q->consumer_read % packets];
  return 1;
}
extern "C" void recomp_audio_packet_advance(RecompAudioPacketSession *q) {
  if (q && q->consumer_read != q->write.load(std::memory_order_acquire))
    q->advance();
}
extern "C" void recomp_audio_packet_prune(RecompAudioPacketSession *q,
                                          uint64_t now, uint32_t queued) {
  if (q)
    q->prune(q->write.load(std::memory_order_acquire), now, queued);
}
extern "C" int recomp_audio_packet_ready(RecompAudioPacketSession *q,
                                         size_t requested, uint64_t now) {
  if (!q)
    return 0;
  if (!q->priming)
    return 1;
  if (!requested)
    return 0;
  const uint64_t reserve =
      (uint64_t(q->rate) * prime_ms + 999) / 1000 * q->channels;
  const uint64_t target = reserve + std::min<uint64_t>(requested, reserve);
  const size_t published = q->write.load(std::memory_order_acquire);
  const bool full = published - q->consumer_read == packets;
  q->prune(published, now, 0);
  uint64_t available = 0;
  for (size_t i = 0; i < published - q->consumer_read; ++i) {
    available += q->slots[(q->consumer_read + i) % packets].samples;
    if (available >= target)
      break;
  }
  q->priming = available < target && !full;
  return !q->priming;
}
extern "C" void recomp_audio_packet_fill(RecompAudioPacketSession *q,
                                         const RecompAudioSink *sink,
                                         void *stream, size_t requested,
                                         uint64_t started) {
  if (!q || !sink || !sink->info || !sink->put)
    return;
  if (recomp_audio_packet_ready(q, requested, started)) {
    while (requested) {
      const size_t published = q->write.load(std::memory_order_acquire);
      if (q->consumer_read == published)
        break;
      const auto before =
          sink->info(sink->user, stream, RECOMP_AUDIO_QUERY_QUEUED);
      q->prune(published, started,
               !byte_count_valid(before.queued_bytes)
                   ? 0
                   : uint32_t(before.queued_bytes) / (2 * q->channels));
      const auto &p = q->slots[q->consumer_read % packets];
      RecompAudioSubmission s{};
      s.dequeued_ns = q->now();
      const auto available =
          sink->info(sink->user, stream, RECOMP_AUDIO_QUERY_AVAILABLE);
      s.put_started_ns = q->now();
      s.succeeded = sink->put(sink->user, stream, p.pcm, p.samples);
      s.put_completed_ns = q->now();
      const auto after =
          sink->info(sink->user, stream,
                     RECOMP_AUDIO_QUERY_QUEUED | RECOMP_AUDIO_QUERY_AVAILABLE);
      if (byte_count_valid(before.queued_bytes)) {
        s.valid |= 1;
        s.queued_before_frames =
            uint32_t(before.queued_bytes) / (2 * q->channels);
      }
      if (byte_count_valid(after.queued_bytes)) {
        s.valid |= 2;
        s.queued_after_frames =
            uint32_t(after.queued_bytes) / (2 * q->channels);
      }
      if (byte_count_valid(available.available_bytes)) {
        s.valid |= 4;
        s.available_before_bytes = uint32_t(available.available_bytes);
      }
      if (byte_count_valid(after.available_bytes)) {
        s.valid |= 8;
        s.available_after_bytes = uint32_t(after.available_bytes);
      }
      if (q->observer.submitted)
        q->observer.submitted(q->observer.user, p.sequence, &s);
      requested = requested > p.samples ? requested - p.samples : 0;
      q->advance();
    }
    const auto final =
        sink->info(sink->user, stream, RECOMP_AUDIO_QUERY_QUEUED);
    q->queued_frames.store(!byte_count_valid(final.queued_bytes)
                               ? 0
                               : uint32_t(final.queued_bytes) /
                                     (2 * q->channels),
                           std::memory_order_relaxed);
  }
  const auto end = q->now();
  q->submit_ns.fetch_add(end > started ? end - started : 0,
                         std::memory_order_relaxed);
}
extern "C" void recomp_audio_packet_discard(RecompAudioPacketSession *q,
                                            int reason) {
  if (!q)
    return;
  RecompAudioPacket p{};
  while (recomp_audio_packet_peek(q, &p)) {
    q->discard(p.sequence, reason);
    q->advance();
  }
  q->priming = q->paced;
  q->queued_frames.store(0, std::memory_order_relaxed);
}
extern "C" void
recomp_audio_packet_discard_notify(RecompAudioPacketSession *q, void *user,
                                   void (*notify)(void *, uint64_t)) {
  if (!q)
    return;
  RecompAudioPacket p{};
  while (recomp_audio_packet_peek(q, &p)) {
    if (notify)
      notify(user, p.sequence);
    q->advance();
  }
  q->priming = q->paced;
  q->queued_frames.store(0, std::memory_order_relaxed);
}
extern "C" uint32_t
recomp_audio_packet_queued_frames(const RecompAudioPacketSession *q) {
  return q ? q->queued_frames.load(std::memory_order_relaxed) : 0;
}
extern "C" uint64_t
recomp_audio_packet_take_submit_ns(RecompAudioPacketSession *q) {
  return q ? q->submit_ns.exchange(0, std::memory_order_relaxed) : 0;
}
extern "C" size_t
recomp_audio_packet_read_position(const RecompAudioPacketSession *q) {
  return q ? q->read.load(std::memory_order_acquire) : 0;
}
extern "C" int recomp_audio_packet_priming(const RecompAudioPacketSession *q) {
  return q ? q->priming : 0;
}

extern "C" RecompAudioPull
recomp_audio_observe_pull(const RecompAudioStreamInfo *s, int32_t total,
                          int *unit) {
  RecompAudioPull p{};
  if (!s || !unit || !s->format_available || s->source_hz <= 0 ||
      s->destination_hz <= 0) {
    if (unit)
      *unit = 0;
    p.reason = RECOMP_AUDIO_FORMAT_UNAVAILABLE;
    return p;
  }
  p.source_hz = s->source_hz;
  p.destination_hz = s->destination_hz;
  if (s->source_hz != s->destination_hz || s->frequency_ratio != 1.f) {
    *unit = 0;
    p.reason = RECOMP_AUDIO_RESAMPLING;
    return p;
  }
  if (!*unit) {
    p.reason = RECOMP_AUDIO_PRIOR_RATE_UNOBSERVED;
    return p;
  }
  if (total < 0 || !s->source_frame_bytes ||
      uint32_t(total) % s->source_frame_bytes) {
    p.reason = RECOMP_AUDIO_INVALID_REQUEST;
    return p;
  }
  if (s->available_bytes < 0) {
    p.reason = RECOMP_AUDIO_AVAILABILITY_UNAVAILABLE;
    return p;
  }
  if (!s->destination_frame_bytes ||
      uint64_t(s->available_bytes) > UINT32_MAX ||
      uint32_t(s->available_bytes) % s->destination_frame_bytes) {
    p.reason = RECOMP_AUDIO_INVALID_AVAILABILITY;
    return p;
  }
  p.requested_frames = uint32_t(total) / s->source_frame_bytes;
  p.available_frames =
      uint32_t(s->available_bytes) / s->destination_frame_bytes;
  return p;
}
namespace {
struct Playback {
  RecompAudioBackend backend{};
  RecompAudioSource source{};
  void *stream = nullptr;
  int unit_rate_only = 1;
  ~Playback() {
    if (stream) {
      backend.detach(backend.sink.user, stream);
      backend.destroy(backend.sink.user, stream);
      if (source.discard)
        source.discard(source.user, RECOMP_AUDIO_CLOSED);
    }
    if (source.destroy)
      source.destroy(source.user);
  }
  static void fill(void *user, void *stream, int32_t additional,
                   int32_t total) {
    if (!stream)
      return;
    auto &p = *static_cast<Playback *>(user);
    auto &b = p.backend;
    b.set_gain(b.sink.user, stream, b.gain(b.sink.user));
    p.source.fill(p.source.user, stream,
                  size_t(std::max(additional, 0)) / sizeof(int16_t));
    auto info = b.sink.info(b.sink.user, stream, RECOMP_AUDIO_QUERY_FORMAT);
    if (info.format_available && info.source_hz > 0 &&
        info.destination_hz > 0 && info.source_hz == info.destination_hz &&
        info.frequency_ratio == 1.f && p.unit_rate_only)
      info.available_bytes =
          b.sink.info(b.sink.user, stream, RECOMP_AUDIO_QUERY_AVAILABLE)
              .available_bytes;
    const auto pull =
        recomp_audio_observe_pull(&info, total, &p.unit_rate_only);
    if (p.source.observe_pull)
      p.source.observe_pull(p.source.user, &pull);
  }
};
} // namespace
struct RecompAudioOutputSession {
  RecompAudioBackend backend{};
  std::unique_ptr<Playback> current;
};
extern "C" RecompAudioOutputSession *
recomp_audio_output_create(const RecompAudioBackend *b) {
  if (!b || !b->sink.info || !b->sink.put || !b->open || !b->resume ||
      !b->set_gain || !b->gain || !b->lock || !b->unlock || !b->clear ||
      !b->detach || !b->destroy)
    return nullptr;
  auto *out = new (std::nothrow) RecompAudioOutputSession;
  if (out)
    out->backend = *b;
  return out;
}
extern "C" void recomp_audio_output_destroy(RecompAudioOutputSession *out) {
  delete out;
}
extern "C" int recomp_audio_output_open(RecompAudioOutputSession *out,
                                        uint32_t rate, uint32_t channels,
                                        RecompAudioSource source) {
  std::unique_ptr<Playback> p(new (std::nothrow) Playback);
  if (!p) {
    if (source.destroy)
      source.destroy(source.user);
    return 0;
  }
  p->source = source;
  if (!out || !source.fill || !rate || rate > INT32_MAX || !channels ||
      channels > 2)
    return 0;
  p->backend = out->backend;
  auto &b = p->backend;
  p->stream = b.open(b.sink.user, rate, channels, Playback::fill, p.get());
  if (!p->stream || !b.set_gain(b.sink.user, p->stream, b.gain(b.sink.user)) ||
      !b.resume(b.sink.user, p->stream))
    return 0;
  out->current = std::move(p);
  return 1;
}
extern "C" int recomp_audio_output_clear(RecompAudioOutputSession *out) {
  if (!out || !out->current)
    return 1;
  auto &p = *out->current;
  auto &b = p.backend;
  if (!b.lock(b.sink.user, p.stream))
    return 0;
  if (p.source.discard)
    p.source.discard(p.source.user, RECOMP_AUDIO_CLEARED);
  const int ok = b.clear(b.sink.user, p.stream);
  b.unlock(b.sink.user, p.stream);
  return ok;
}
extern "C" void *
recomp_audio_output_stream(const RecompAudioOutputSession *out) {
  return out && out->current ? out->current->stream : nullptr;
}
