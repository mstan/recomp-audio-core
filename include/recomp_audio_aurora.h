/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Stanley
 * Optional GPL Aurora delivery profile. Link recomp-audio::aurora-gpl only
 * when its GPL backend/dependency terms are applicable to the consumer.
 */
#ifndef RECOMP_AUDIO_AURORA_H
#define RECOMP_AUDIO_AURORA_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t producer_frames,requested_bytes,missing_bytes,underrun_requests;
    uint64_t pushes,dropped_frames,low_queue_pushes,stretched_pushes;
    uint32_t sample_rate,queued_bytes,playing,failed,muted;
    char driver[32];
} RecompAudioAuroraStats;
typedef struct {
    bool muted,unpaced,mute_output_device;
    const char *hitch_trace_path; /* copied at start; optional exclusive export */
} RecompAudioAuroraOptions;
/* One owner thread and one active process profile, matching the upstream
 * backend's globals. PCM/format/control calls are owner-thread operations;
 * the SDL callback observes demand only. Stop joins/detaches device work. */
bool recomp_audio_aurora_start(const RecompAudioAuroraOptions *);
void recomp_audio_aurora_stop(void);
void recomp_audio_aurora_push(const int16_t *,uint32_t frames);
void recomp_audio_aurora_set_rate(uint32_t rate);
void recomp_audio_aurora_suppress(bool);
bool recomp_audio_aurora_present(void);
bool recomp_audio_aurora_pause(void);
bool recomp_audio_aurora_discontinuity(uint32_t rate);
#ifdef RECOMP_AUDIO_AURORA_ROLLBACK
void recomp_audio_aurora_replay(bool active,uint32_t restored_rate);
#endif
uint64_t recomp_audio_aurora_submit_ns(void);
RecompAudioAuroraStats recomp_audio_aurora_stats(void);
/* Guarded donor adaptation hooks; not a product backend selector. */
bool recomp_audio_aurora_unpaced(void);
bool recomp_audio_aurora_observe_stream(void *);
#ifdef __cplusplus
}
#endif
#endif
