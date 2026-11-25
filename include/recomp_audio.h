#ifndef RECOMP_AUDIO_H
#define RECOMP_AUDIO_H
#include "recomp_audio_drc.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct recomp_audio recomp_audio;
enum { RECOMP_AUDIO_GENESIS = 0, RECOMP_AUDIO_LEGACY_NES = 1 };
/* The profile is fixed at creation. All operations on one instance, including
 * observation/reset/destruction, require caller-provided exclusion. This is
 * not a lock-free SPSC queue: producer overflow also moves the read cursor. */
recomp_audio *recomp_audio_create(const rab_config *config);
void recomp_audio_destroy(recomp_audio *audio);
void recomp_audio_reset(recomp_audio *audio);
void recomp_audio_push(recomp_audio *audio, const int16_t *samples, int frames);
void recomp_audio_pull(recomp_audio *audio, int16_t *samples, int frames);
double recomp_audio_fill_ms(const recomp_audio *audio);
void recomp_audio_stats(const recomp_audio *audio, rab_stats *stats);
#ifdef __cplusplus
}
#endif
#endif
