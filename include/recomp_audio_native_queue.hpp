/* NDS native PCM delivery profile. Console synthesis and device ownership
 * remain with the caller. The caller serializes ring access against its audio
 * callback. This header has no SDL/platform dependency. */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace recomp_audio_native {
enum class Push { Full, Done, Failed };

template<uint32_t Capacity> struct StereoRing {
    std::array<int16_t, Capacity * 2u> samples{};
    uint32_t read = 0, write = 0, count = 0;
    uint32_t pull(int16_t* output, uint32_t requested) {
        const uint32_t take = std::min(requested, count);
        const uint32_t first = std::min(take, Capacity - read);
        std::memcpy(output, samples.data() + read * 2u, first * 4u);
        if (first < take)
            std::memcpy(output + first * 2u, samples.data(), (take - first) * 4u);
        read = (read + take) % Capacity;
        count -= take;
        return take;
    }
    bool push(const int16_t* input, uint32_t frames) {
        if (Capacity - count < frames) return false;
        const uint32_t first = std::min(frames, Capacity - write);
        std::memcpy(samples.data() + write * 2u, input, first * 4u);
        if (first < frames)
            std::memcpy(samples.data(), input + first * 2u, (frames - first) * 4u);
        write = (write + frames) % Capacity;
        count += frames;
        return true;
    }
    void reset() { read = write = count = 0; }
};

/* Source fills at most1024 stereo frames. Sink reports count(), try_push()
 * and wait() with the device's existing format, synchronization and wait.
 * Full means wait only when throttling; no produced block is dropped. The
 * audio queue is the frame clock, including the caller's boot runway floor. */
template<class Sink, class Source>
uint32_t drain(Sink& sink, Source source, bool throttle,
               uint32_t pace_floor, bool& queue_error) {
    std::array<int16_t, 2048> samples{};
    for (;;) {
        const uint32_t frames = source(samples.data(), 1024);
        if (!frames) break;
        for (;;) {
            const Push result = sink.try_push(samples.data(), frames);
            if (result == Push::Done) break;
            if (result == Push::Failed || !throttle) {
                queue_error = true;
                return sink.count();
            }
            sink.wait();
        }
    }
    uint32_t queued = sink.count();
    while (throttle && queued > pace_floor) {
        sink.wait();
        queued = sink.count();
    }
    return queued;
}
} // namespace recomp_audio_native
