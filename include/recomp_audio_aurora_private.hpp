// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthew Stanley
// Audio-only ABI declarations for the optional GPL backend.
#pragma once
#include <cstdint>
#include <SDL3/SDL_audio.h>
#include <atomic>
namespace gx_aurora {
extern bool g_audio_queue_log,g_audio_playing;
extern unsigned long long g_audio_push_count,g_audio_throttle_count,g_audio_dropped_count,g_audio_dropped_frames;
extern unsigned long long g_audio_low_log_push,g_audio_starved_count,g_audio_stretched_count;
extern std::atomic_bool g_audio_discard;
extern uint32_t g_audio_sample_rate;
extern SDL_AudioStream* g_audio_stream;
extern int g_audio_prebuffer_ms,g_audio_max_queue_ms;
void close_audio_capture();
void recover_audio_output();
bool retry_audio_open(bool force);
}
extern "C" {
void aurora_backend_audio_set_sample_rate(uint32_t);
void aurora_backend_audio_push(const int16_t*,uint32_t);
void dol_aurora_audio_resume(void);
}
