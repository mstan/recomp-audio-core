#ifndef AUDIO_ORACLE_API_H
#define AUDIO_ORACLE_API_H
#include <stdint.h>
typedef struct {
    uint64_t in_count, ring_hash, pushed, pulled, underruns, overflow, stretch, episodes;
    double out_pos, step, err, corr, gain, prime, loop_start, loop_len, loop_pos, fill, correction;
    float last_out[2];
    int primed, concealing;
} oracle_state;
void *oracle_create(int profile, double source, double host, int channels, int stretch);
void oracle_destroy(int profile, void *context);
void oracle_push(int profile, void *context, const int16_t *samples, int frames);
void oracle_pull(int profile, void *context, int16_t *samples, int frames);
void oracle_observe(int profile, void *context, oracle_state *state);
void oracle_reset(int profile, void *context);
#endif
