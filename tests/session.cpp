// SPDX-License-Identifier: MIT
#include "recomp_audio_session.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

struct Observation {
  uint64_t next = 0, now = 0;
  std::vector<uint64_t> returned, submitted, discarded;
  std::vector<int> reasons;
};
static uint64_t clock_ns(void *p) {
  return ++static_cast<Observation *>(p)->now;
}
static uint64_t produced(void *p, size_t, uint64_t, uint64_t) {
  return ++static_cast<Observation *>(p)->next;
}
static void returned(void *p, uint64_t seq) {
  static_cast<Observation *>(p)->returned.push_back(seq);
}
static void discarded(void *p, uint64_t seq, int reason, int) {
  auto &o = *static_cast<Observation *>(p);
  o.discarded.push_back(seq);
  o.reasons.push_back(reason);
}
static void submitted(void *p, uint64_t seq, const RecompAudioSubmission *s) {
  assert(s->succeeded && s->valid == 15);
  static_cast<Observation *>(p)->submitted.push_back(seq);
}
static RecompAudioPacketObserver observer(Observation &o) {
  return {&o, clock_ns, produced, returned, discarded, submitted};
}
struct Sink {
  std::vector<int16_t> pcm;
  unsigned queries[8]{};
  static RecompAudioStreamInfo info(void *p, void *, uint32_t fields) {
    auto &s = *static_cast<Sink *>(p);
    ++s.queries[fields];
    RecompAudioStreamInfo i{};
    i.queued_bytes = int64_t(s.pcm.size() * 2);
    i.available_bytes = i.queued_bytes;
    i.source_hz = i.destination_hz = 48000;
    i.source_frame_bytes = i.destination_frame_bytes = 4;
    i.frequency_ratio = 1;
    i.format_available = 1;
    return i;
  }
  static int put(void *p, void *, const int16_t *pcm, size_t samples) {
    auto &s = *static_cast<Sink *>(p);
    s.pcm.insert(s.pcm.end(), pcm, pcm + samples);
    return 1;
  }
  RecompAudioSink api() { return {this, info, put}; }
};
static void packets() {
  Observation o;
  auto hooks = observer(o);
  auto *q = recomp_audio_packet_create(48000, 2, 0, &hooks);
  assert(q);
  int16_t pair[2]{};
  assert(recomp_audio_packet_submit(q, pair, 1, 0, 0) == -1);
  assert(!o.next);
  for (unsigned n = 0; n < 17; ++n) {
    pair[0] = int16_t(n);
    pair[1] = -int16_t(n);
    assert(recomp_audio_packet_submit(q, pair, 2, n, n) == int(n == 16));
  }
  assert(o.returned.size() == 17 && o.discarded == std::vector<uint64_t>{17} &&
         o.reasons[0] == RECOMP_AUDIO_UNPACED_OVERFLOW);
  for (unsigned n = 0; n < 16; ++n) {
    RecompAudioPacket p{};
    assert(recomp_audio_packet_peek(q, &p));
    assert(p.sequence == n + 1 && p.samples == 2 && p.pcm[0] == int(n) &&
           p.pcm[1] == -int(n));
    recomp_audio_packet_advance(q);
  }
  RecompAudioPacket p{};
  assert(!recomp_audio_packet_peek(q, &p));
  recomp_audio_packet_submit(q, pair, 2, 20, 20);
  recomp_audio_packet_discard(q, RECOMP_AUDIO_CLEARED);
  assert(!recomp_audio_packet_peek(q, &p));
  recomp_audio_packet_destroy(q);
}
static void pacing() {
  Observation o;
  auto hooks = observer(o);
  auto *q = recomp_audio_packet_create(48000, 2, 1, &hooks);
  assert(q);
  std::vector<int16_t> field(1920, 1);
  assert(!recomp_audio_packet_ready(q, 960, 0));
  recomp_audio_packet_submit(q, field.data(), field.size(), 1, 0);
  assert(!recomp_audio_packet_ready(q, 960, 5000000));
  recomp_audio_packet_submit(q, field.data(), field.size(), 2, 20000000);
  assert(recomp_audio_packet_ready(q, 960, 21000000));
  recomp_audio_packet_discard(q, RECOMP_AUDIO_CLEARED);
  assert(!recomp_audio_packet_ready(q, 960, 21000000));
  for (unsigned n = 0; n < 16; ++n)
    recomp_audio_packet_submit(q, field.data(), 2, n, 21000000);
  assert(recomp_audio_packet_ready(q, 960, 21000000));
  recomp_audio_packet_destroy(q);
  q = recomp_audio_packet_create(48000, 2, 1, &hooks);
  std::vector<int16_t> old(2048, 11), fresh(2048, 29);
  recomp_audio_packet_submit(q, old.data(), old.size(), 1, 0);
  recomp_audio_packet_submit(q, fresh.data(), fresh.size(), 2, 19000000);
  recomp_audio_packet_prune(q, 20000000, 1440);
  assert(recomp_audio_packet_read_position(q) == 1);
  recomp_audio_packet_prune(q, 240000000, 0);
  RecompAudioPacket last{};
  assert(recomp_audio_packet_peek(q, &last) && last.pcm[0] == 29);
  recomp_audio_packet_destroy(q);
  o = Observation{};
  q = recomp_audio_packet_create(48000, 2, 1, &hooks);
  const uint64_t times[] = {40000000, 56000000, 276000000, 293000000};
  for (unsigned n = 0; n < 4; ++n) {
    std::vector<int16_t> p(2048, int16_t(n + 1));
    recomp_audio_packet_submit(q, p.data(), p.size(), n + 1, times[n]);
  }
  Sink sink;
  auto output = sink.api();
  recomp_audio_packet_fill(q, &output, nullptr, 4096, 300000000);
  assert(o.discarded == std::vector<uint64_t>({1, 2}) &&
         o.submitted == std::vector<uint64_t>({3, 4}));
  assert(sink.pcm.size() == 4096 && sink.pcm.front() == 3 &&
         sink.pcm[2048] == 4);
  assert(recomp_audio_packet_queued_frames(q) == 2048 &&
         recomp_audio_packet_read_position(q) == 4);
  // Queue delivery asks only for the observations it consumes, with no format
  // query.
  assert(
      !sink.queries[RECOMP_AUDIO_QUERY_FORMAT] &&
      !sink.queries[RECOMP_AUDIO_QUERY_FORMAT | RECOMP_AUDIO_QUERY_AVAILABLE]);
  recomp_audio_packet_destroy(q);
}
struct Backend {
  struct Stream {
    RecompAudioPullCallback callback;
    void *user;
    float gain = 1;
    bool locked = false, detached = false;
  };
  Sink sink;
  bool fail_open = false, fail_resume = false, fail_lock = false;
  unsigned detach_count = 0, destroy_count = 0;
  static void *open(void *p, uint32_t, uint32_t, RecompAudioPullCallback cb,
                    void *user) {
    auto &b = *static_cast<Backend *>(p);
    return b.fail_open ? nullptr : new Stream{cb, user};
  }
  static int resume(void *p, void *s) {
    assert(static_cast<Stream *>(s)->gain == .375f);
    return !static_cast<Backend *>(p)->fail_resume;
  }
  static int gain(void *, void *s, float gain) {
    static_cast<Stream *>(s)->gain = gain;
    return 1;
  }
  static float saved_gain(void *) { return .375f; }
  static int lock(void *p, void *s) {
    if (static_cast<Backend *>(p)->fail_lock)
      return 0;
    static_cast<Stream *>(s)->locked = true;
    return 1;
  }
  static void unlock(void *, void *s) {
    assert(static_cast<Stream *>(s)->locked);
    static_cast<Stream *>(s)->locked = false;
  }
  static int clear(void *, void *s) {
    assert(static_cast<Stream *>(s)->locked);
    return 1;
  }
  static void detach(void *p, void *s) {
    static_cast<Stream *>(s)->detached = true;
    ++static_cast<Backend *>(p)->detach_count;
  }
  static void destroy(void *p, void *s) {
    assert(static_cast<Stream *>(s)->detached);
    ++static_cast<Backend *>(p)->destroy_count;
    delete static_cast<Stream *>(s);
  }
  static RecompAudioStreamInfo info(void *p, void *s, uint32_t flags) {
    return Sink::info(&static_cast<Backend *>(p)->sink, s, flags);
  }
  static int put(void *p, void *s, const int16_t *pcm, size_t n) {
    return Sink::put(&static_cast<Backend *>(p)->sink, s, pcm, n);
  }
  RecompAudioBackend api() {
    return {{this, info, put},
            open,
            resume,
            gain,
            saved_gain,
            lock,
            unlock,
            clear,
            detach,
            destroy};
  }
};
struct Source {
  Backend *backend;
  unsigned fill = 0, clears = 0, closes = 0, destroys = 0, pulls = 0;
  RecompAudioSource api() {
    return {this,
            [](void *p, void *, size_t n) {
              assert(n == 16);
              ++static_cast<Source *>(p)->fill;
            },
            [](void *p, const RecompAudioPull *pull) {
              assert(pull->reason == RECOMP_AUDIO_OBSERVED);
              ++static_cast<Source *>(p)->pulls;
            },
            [](void *p, int reason) {
              auto &s = *static_cast<Source *>(p);
              if (reason == RECOMP_AUDIO_CLEARED)
                ++s.clears;
              else {
                assert(s.backend->destroy_count > s.closes);
                ++s.closes;
              }
            },
            [](void *p) { ++static_cast<Source *>(p)->destroys; }};
  }
};
static void lifecycle() {
  Backend b;
  auto backend = b.api();
  auto *out = recomp_audio_output_create(&backend);
  assert(out);
  Source one{&b}, two{&b}, bad{&b};
  assert(recomp_audio_output_open(out, 48000, 2, one.api()));
  auto *first = static_cast<Backend::Stream *>(recomp_audio_output_stream(out));
  assert(first);
  first->callback(first->user, first, 32, 64);
  assert(one.fill == 1 && one.pulls == 1);
  assert(recomp_audio_output_clear(out) && one.clears == 1);
  b.fail_lock = true;
  assert(!recomp_audio_output_clear(out) && one.clears == 1);
  b.fail_lock = false;
  b.fail_open = true;
  assert(!recomp_audio_output_open(out, 48000, 2, bad.api()) &&
         bad.destroys == 1 && !bad.closes);
  assert(recomp_audio_output_stream(out) == first);
  b.fail_open = false;
  b.fail_resume = true;
  assert(!recomp_audio_output_open(out, 48000, 2, bad.api()) &&
         bad.destroys == 2 && bad.closes == 1);
  assert(recomp_audio_output_stream(out) == first);
  b.fail_resume = false;
  assert(recomp_audio_output_open(out, 48000, 2, two.api()) &&
         one.closes == 1 && one.destroys == 1);
  recomp_audio_output_destroy(out);
  assert(two.closes == 1 && two.destroys == 1 && b.detach_count == 3 &&
         b.destroy_count == 3);
  RecompAudioStreamInfo info{};
  info.source_hz = info.destination_hz = 48000;
  info.source_frame_bytes = 4;
  info.destination_frame_bytes = 8;
  info.available_bytes = 3840;
  info.format_available = 1;
  info.frequency_ratio = 1;
  int unit = 1;
  auto p = recomp_audio_observe_pull(&info, 2048, &unit);
  assert(!p.reason && p.requested_frames == 512 && p.available_frames == 480);
  info.destination_hz = 44100;
  assert(recomp_audio_observe_pull(&info, 2048, &unit).reason ==
             RECOMP_AUDIO_RESAMPLING &&
         !unit);
  info.destination_hz = 48000;
  assert(recomp_audio_observe_pull(&info, 2048, &unit).reason ==
         RECOMP_AUDIO_PRIOR_RATE_UNOBSERVED);
}
static void concurrent() {
  auto *q = recomp_audio_packet_create(48000, 2, 0, nullptr);
  assert(q);
  std::atomic_bool done{false};
  unsigned accepted = 0, dropped = 0, consumed = 0;
  uint64_t last = 0;
  std::thread producer([&] {
    for (unsigned n = 0; n < 10000; ++n) {
      int16_t p[] = {int16_t(n), int16_t(-int(n))};
      int result = recomp_audio_packet_submit(q, p, 2, n, n);
      assert(result >= 0);
      if (result)
        ++dropped;
      else
        ++accepted;
    }
    done.store(true, std::memory_order_release);
  });
  for (;;) {
    RecompAudioPacket p{};
    if (recomp_audio_packet_peek(q, &p)) {
      assert(p.sequence > last && p.samples == 2 &&
             p.pcm[0] == int(p.sequence - 1) &&
             p.pcm[1] == -int(p.sequence - 1));
      last = p.sequence;
      ++consumed;
      recomp_audio_packet_advance(q);
    } else if (done.load(std::memory_order_acquire) &&
               !recomp_audio_packet_peek(q, &p))
      break;
    else
      std::this_thread::yield();
  }
  producer.join();
  assert(accepted + dropped == 10000 && consumed == accepted);
  recomp_audio_packet_destroy(q);
}
int main() {
  packets();
  pacing();
  lifecycle();
  concurrent();
}
