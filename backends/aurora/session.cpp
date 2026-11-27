// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthew Stanley. Owner-authored session glue; the
// separately compiled Chris Sotraidis donor backend retains GPL-3.0-or-later.
#include "recomp_audio_aurora.h"
#include "recomp_audio_pcm_batch.hpp"
#include "recomp_audio_aurora_private.hpp"
#include <string>
#include <utility>
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using u32=uint32_t; using s16=int16_t;

namespace gx_aurora {
bool g_audio_queue_log=false;
unsigned long long g_audio_push_count{},g_audio_throttle_count{};
unsigned long long g_audio_dropped_count{},g_audio_dropped_frames{};
unsigned long long g_audio_low_log_push{},g_audio_starved_count{},g_audio_stretched_count{};
std::atomic_bool g_audio_discard{false};
u32 g_audio_sample_rate=32000;
SDL_AudioStream* g_audio_stream{};
bool g_audio_playing{};
int g_audio_prebuffer_ms=40,g_audio_max_queue_ms=250;
}
namespace {
bool installed{},muted{},mute_output_device{},unpaced{},failed{},boot_suppressed{};
#ifdef RECOMP_AUDIO_AURORA_ROLLBACK
bool replaying{};
#endif
uint64_t producer_frames{};
uint64_t submit_ns{};
recomp_audio_native::StereoBatch<64> pending;
SDL_AudioStream* observed{};
std::atomic_uint64_t requested{},missing{},underruns{};
struct ShortAudioRequest {
    uint64_t time_ns;
    int additional;
    int total;
};
constexpr size_t short_request_capacity = 4096;
std::array<ShortAudioRequest,short_request_capacity> short_requests;
std::atomic_size_t short_request_count{};
const char* short_request_path{};
std::string short_request_storage;
/* SDL reports the input bytes needed for the current output request. This
 * observer never supplies substitute data, advances guest time or changes
 * emulation. Resampling can slightly overestimate byte counts; event count
 * remains distinct from the donor's <10ms queue heuristic. */
void SDLCALL requested_data(void*,SDL_AudioStream*,int additional,int total) {
    if (total>0) requested.fetch_add(uint64_t(total),std::memory_order_relaxed);
    if (additional>0) {
        missing.fetch_add(uint64_t(additional),std::memory_order_relaxed);
        underruns.fetch_add(1,std::memory_order_relaxed);
        if (short_request_path) {
            const auto index=short_request_count.fetch_add(1,std::memory_order_relaxed);
            if (index<short_requests.size()) {
                const auto now=std::chrono::steady_clock::now().time_since_epoch();
                short_requests[index]={
                    uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()),
                    additional,total};
            }
        }
    }
}
void submit_pcm(const s16* samples,u32 frames) {
    if (!short_request_path) {
        aurora_backend_audio_push(samples,frames);
        return;
    }
    const auto start=std::chrono::steady_clock::now();
    aurora_backend_audio_push(samples,frames);
    submit_ns+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count());
}
void submit_pending() { pending.flush(submit_pcm); }
void push(const s16* samples,u32 frames) {
#ifdef RECOMP_AUDIO_AURORA_ROLLBACK
    if(replaying)return;
#endif
    if (!samples || !frames) return;
    producer_frames+=frames;
    if (boot_suppressed) return;
    pending.push(samples,frames,submit_pcm);
}
void set_rate(u32 rate) {
#ifdef RECOMP_AUDIO_AURORA_ROLLBACK
    if(replaying)return;
#endif
    const u32 next=rate==48000 ? 48000 : 32000;
    if (next==gx_aurora::g_audio_sample_rate) return;
    submit_pending();
    aurora_backend_audio_set_sample_rate(next);
}
}
extern "C" bool recomp_audio_aurora_unpaced(void) {return unpaced;}
extern "C" uint64_t recomp_audio_aurora_submit_ns(void) {return submit_ns;}
extern "C" bool recomp_audio_aurora_observe_stream(void* value) {
    auto* stream=static_cast<SDL_AudioStream*>(value);
    if (!stream) return true;
    if ((mute_output_device && !SDL_SetAudioStreamGain(stream,0.0f)) ||
        !SDL_SetAudioStreamGetCallback(stream,requested_data,nullptr)) {
        failed=true;
        return false;
    }
    observed=stream;
    return true;
}
extern "C" bool recomp_audio_aurora_start(const RecompAudioAuroraOptions* options) {
    const RecompAudioAuroraOptions config=options ? *options : RecompAudioAuroraOptions{};
    const bool silent=config.muted,unlimited=config.unpaced;
    if (installed) return false;
    /* A process-local SDL hint is set before audio initialization. Owner
     * handoffs omit this flag and use their normal playback device. */
    std::string trace;
    try { trace=config.hitch_trace_path ? config.hitch_trace_path : ""; }
    catch (...) { return false; }
    mute_output_device=silent && config.mute_output_device;
    if (silent && !mute_output_device && (SDL_WasInit(SDL_INIT_AUDIO) ||
        !SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER,"dummy",SDL_HINT_OVERRIDE))) return false;
    muted=silent;unpaced=unlimited;failed=false;boot_suppressed=false;producer_frames=0;pending.reset();observed=nullptr;
    requested=0;missing=0;underruns=0;submit_ns=0;
    short_request_storage=std::move(trace);
    short_request_path=short_request_storage.empty() ? nullptr : short_request_storage.c_str();
    short_request_count=0;
    using namespace gx_aurora;
    g_audio_push_count=g_audio_throttle_count=g_audio_dropped_count=g_audio_dropped_frames=0;
    g_audio_low_log_push=g_audio_starved_count=g_audio_stretched_count=0;
    g_audio_sample_rate=32000;g_audio_playing=false;g_audio_discard=false;
    installed=true;return true;
}
extern "C" void recomp_audio_aurora_suppress(bool suppressed) {
    boot_suppressed=suppressed;
}
extern "C" bool recomp_audio_aurora_present(void) {
    /* Drain a short tail even if the guest stops sending AID transfers. */
    submit_pending();
    using namespace gx_aurora;
    if (failed || unpaced || !g_audio_playing || !g_audio_stream) return !failed;

    /* Backpressure at the queue limit protects bounded audio latency. The VI
     * presentation deadline normally keeps the producer at the guest rate. */
    const int bytes_per_second = static_cast<int>(g_audio_sample_rate) * 2 * sizeof(s16);
    const int max_queued = bytes_per_second * g_audio_max_queue_ms / 1000;
    const int queued = SDL_GetAudioStreamQueued(g_audio_stream);
    if (queued > max_queued && !SDL_AudioStreamDevicePaused(g_audio_stream)) {
        ++g_audio_throttle_count;
        SDL_DelayPrecise(uint64_t(queued - max_queued) * 1000000000ull / bytes_per_second);
    }
    return true;
}
extern "C" bool recomp_audio_aurora_pause(void) {

    using namespace gx_aurora;
    if (!g_audio_playing || !g_audio_stream) return !failed;
    if (!SDL_PauseAudioStreamDevice(g_audio_stream)) return false;
    // Keep queued PCM. The existing sink re-primes to its prebuffer size
    // before resuming the physical device on the next active guest push.
    g_audio_playing=false;
    return !failed;
}

extern "C" bool recomp_audio_aurora_discontinuity(uint32_t sample_rate) {
    pending.reset();

    auto* stream=gx_aurora::g_audio_stream;
    if(stream && !SDL_ClearAudioStream(stream))return false;
    set_rate(sample_rate);
    return !failed;
}
#ifdef RECOMP_AUDIO_AURORA_ROLLBACK
extern "C" void recomp_audio_aurora_replay(bool active,uint32_t restored_sample_rate) {
    replaying=active;
    if(active) {
        submit_pending();
    } else {

        set_rate(restored_sample_rate);
    }
}
#endif
extern "C" RecompAudioAuroraStats recomp_audio_aurora_stats(void) {
    using namespace gx_aurora;
    RecompAudioAuroraStats result{};
    result.producer_frames=producer_frames;result.requested_bytes=requested.load(std::memory_order_relaxed);
    result.missing_bytes=missing.load(std::memory_order_relaxed);result.underrun_requests=underruns.load(std::memory_order_relaxed);
    result.pushes=g_audio_push_count;result.dropped_frames=g_audio_dropped_frames;
    result.low_queue_pushes=g_audio_starved_count;result.stretched_pushes=g_audio_stretched_count;
    result.sample_rate=g_audio_sample_rate;result.playing=g_audio_playing;result.failed=failed;result.muted=muted;
    if (const char* driver=SDL_GetCurrentAudioDriver()) {
        std::strncpy(result.driver,driver,sizeof(result.driver)-1);
        if (!std::strcmp(driver,"dummy")) result.muted=true;
    }
    const int bytes=g_audio_stream ? SDL_GetAudioStreamQueued(g_audio_stream) : 0;
    if (bytes>0) result.queued_bytes=uint32_t(bytes);
    return result;
}
extern "C" void recomp_audio_aurora_stop(void) {
    if (!installed) return;
    submit_pending();gx_aurora::close_audio_capture();
    if (gx_aurora::g_audio_stream) SDL_DestroyAudioStream(gx_aurora::g_audio_stream);
    gx_aurora::g_audio_stream=nullptr;gx_aurora::g_audio_playing=false;observed=nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);installed=false;
    if (short_request_path) {
        if (auto* trace=std::fopen(short_request_path,"wx")) {
            std::fputs("time_ns,additional_bytes,total_bytes\n",trace);
            const auto count=short_request_count.load(std::memory_order_relaxed);
            for (size_t i=0;i<count && i<short_requests.size();++i) {
                const auto& row=short_requests[i];
                std::fprintf(trace,"%llu,%d,%d\n",static_cast<unsigned long long>(row.time_ns),row.additional,row.total);
            }
            std::fclose(trace);
            if (count>short_requests.size())
                std::fprintf(stderr,"[audio-trace] dropped %zu short requests\n",count-short_requests.size());
        }
        short_request_path=nullptr;
        std::string{}.swap(short_request_storage);
    }
}

extern "C" void recomp_audio_aurora_push(const int16_t* pcm,uint32_t frames) { push(pcm,frames); }
extern "C" void recomp_audio_aurora_set_rate(uint32_t rate) { set_rate(rate); }
