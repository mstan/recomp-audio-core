/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Stanley
 * Native PCM delivery/session owner. No device library or console dependency.
 */
#ifndef RECOMP_AUDIO_SESSION_H
#define RECOMP_AUDIO_SESSION_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompAudioPacketSession RecompAudioPacketSession;
typedef struct RecompAudioOutputSession RecompAudioOutputSession;
enum { RECOMP_AUDIO_PACKET_COUNT = 16, RECOMP_AUDIO_PACKET_SAMPLES = 192000 };
enum RecompAudioDiscard {
  RECOMP_AUDIO_PACED_OVERFLOW = 1,
  RECOMP_AUDIO_UNPACED_OVERFLOW,
  RECOMP_AUDIO_PACED_BACKLOG,
  RECOMP_AUDIO_CLEARED,
  RECOMP_AUDIO_CLOSED
};
enum RecompAudioUnobserved {
  RECOMP_AUDIO_OBSERVED = 0,
  RECOMP_AUDIO_FORMAT_UNAVAILABLE,
  RECOMP_AUDIO_RESAMPLING,
  RECOMP_AUDIO_PRIOR_RATE_UNOBSERVED,
  RECOMP_AUDIO_INVALID_REQUEST,
  RECOMP_AUDIO_AVAILABILITY_UNAVAILABLE,
  RECOMP_AUDIO_INVALID_AVAILABILITY
};
typedef struct {
  uint64_t sequence, produced_ns;
  size_t samples;
  const int16_t *pcm;
} RecompAudioPacket;
typedef struct {
  int64_t queued_bytes, available_bytes; /* negative means unavailable */
  int32_t source_hz, destination_hz;
  uint32_t source_frame_bytes, destination_frame_bytes;
  float frequency_ratio;
  int format_available;
} RecompAudioStreamInfo;
enum {
  RECOMP_AUDIO_QUERY_QUEUED = 1,
  RECOMP_AUDIO_QUERY_AVAILABLE = 2,
  RECOMP_AUDIO_QUERY_FORMAT = 4
};
typedef struct {
  void *user;
  RecompAudioStreamInfo (*info)(void *, void *stream, uint32_t fields);
  int (*put)(void *, void *stream, const int16_t *, size_t samples);
} RecompAudioSink;
typedef struct {
  uint64_t dequeued_ns, put_started_ns, put_completed_ns;
  uint32_t queued_before_frames, queued_after_frames;
  uint32_t available_before_bytes, available_after_bytes;
  uint32_t valid; /* bits 0/1: queued before/after; bits 2/3: available
                     before/after */
  int succeeded;
} RecompAudioSubmission;
typedef struct {
  void *user;
  uint64_t (*clock_ns)(void *);
  uint64_t (*produced)(void *, size_t samples, uint64_t field,
                       uint64_t produced_ns);
  void (*send_returned)(void *, uint64_t sequence);
  void (*discarded)(void *, uint64_t sequence, int reason, int first_overflow);
  void (*submitted)(void *, uint64_t sequence, const RecompAudioSubmission *);
} RecompAudioPacketObserver;
/* One producer and one serialized consumer. Allocation happens only at create.
 * The caller keeps observer.user alive until both endpoints are gone. */
RecompAudioPacketSession *
recomp_audio_packet_create(uint32_t rate, uint32_t channels, int paced,
                           const RecompAudioPacketObserver *);
void recomp_audio_packet_destroy(RecompAudioPacketSession *);
/* -1 invalid; 0 empty/accepted; 1 new packet dropped. Exactly one queue copy.
 */
int recomp_audio_packet_submit(RecompAudioPacketSession *, const int16_t *,
                               size_t, uint64_t field, uint64_t produced_ns);
int recomp_audio_packet_peek(RecompAudioPacketSession *, RecompAudioPacket *);
void recomp_audio_packet_advance(RecompAudioPacketSession *);
void recomp_audio_packet_prune(RecompAudioPacketSession *, uint64_t now_ns,
                               uint32_t device_frames);
int recomp_audio_packet_ready(RecompAudioPacketSession *,
                              size_t requested_samples, uint64_t now_ns);
void recomp_audio_packet_fill(RecompAudioPacketSession *,
                              const RecompAudioSink *, void *stream,
                              size_t requested_samples,
                              uint64_t callback_started_ns);
void recomp_audio_packet_discard(RecompAudioPacketSession *, int reason);
/* Synchronous binding-specific discard reason/observer; no callback is
 * retained. */
void recomp_audio_packet_discard_notify(RecompAudioPacketSession *, void *user,
                                        void (*discarded)(void *,
                                                          uint64_t sequence));
uint32_t recomp_audio_packet_queued_frames(const RecompAudioPacketSession *);
uint64_t recomp_audio_packet_take_submit_ns(RecompAudioPacketSession *);
size_t recomp_audio_packet_read_position(const RecompAudioPacketSession *);
int recomp_audio_packet_priming(const RecompAudioPacketSession *);

typedef struct {
  int32_t source_hz, destination_hz;
  uint32_t requested_frames, available_frames;
  int reason;
} RecompAudioPull;
typedef void (*RecompAudioPullCallback)(void *user, void *stream,
                                        int32_t additional, int32_t total);
/* System-call bindings only. Open returns a paused stream. Detach waits for any
 * active callback; destroy runs while source userdata still exists. */
typedef struct {
  RecompAudioSink sink;
  void *(*open)(void *, uint32_t rate, uint32_t channels,
                RecompAudioPullCallback, void *);
  int (*resume)(void *, void *);
  int (*set_gain)(void *, void *, float);
  float (*gain)(void *);
  int (*lock)(void *, void *);
  void (*unlock)(void *, void *);
  int (*clear)(void *, void *);
  void (*detach)(void *, void *);
  void (*destroy)(void *, void *);
} RecompAudioBackend;
typedef struct {
  void *user;
  void (*fill)(void *, void *stream, size_t requested_samples);
  void (*observe_pull)(void *, const RecompAudioPull *);
  void (*discard)(void *, int reason);
  void (*destroy)(void *);
} RecompAudioSource;
/* Owner-thread API. Source ownership transfers on every open, including error.
 * Failed replacement leaves the previous playback alive. No worker is added. */
RecompAudioOutputSession *
recomp_audio_output_create(const RecompAudioBackend *);
void recomp_audio_output_destroy(RecompAudioOutputSession *);
int recomp_audio_output_open(RecompAudioOutputSession *, uint32_t rate,
                             uint32_t channels, RecompAudioSource);
int recomp_audio_output_clear(RecompAudioOutputSession *);
void *recomp_audio_output_stream(const RecompAudioOutputSession *);
/* Pure observation helper; format/rate uncertainty is sticky per playback. */
RecompAudioPull recomp_audio_observe_pull(const RecompAudioStreamInfo *,
                                          int32_t total_bytes,
                                          int *unit_rate_only);
#ifdef __cplusplus
}
#endif
#endif
