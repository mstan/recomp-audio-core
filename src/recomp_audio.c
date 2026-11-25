#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio.h"
struct recomp_audio { rab_bridge bridge; };
recomp_audio *recomp_audio_create(const rab_config *config) {
    recomp_audio *a = (recomp_audio *)calloc(1, sizeof(*a));
    if (a && rab_init(&a->bridge, config)) { free(a); return NULL; }
    return a;
}
void recomp_audio_destroy(recomp_audio *a) { if (a) { rab_free(&a->bridge); free(a); } }
void recomp_audio_reset(recomp_audio *a) { if (a) rab_reset(&a->bridge); }
void recomp_audio_push(recomp_audio *a, const int16_t *s, int n) {
    if (a && s && n > 0) rab_push(&a->bridge, s, n);
}
void recomp_audio_pull(recomp_audio *a, int16_t *s, int n) {
    if (a && s && n > 0) rab_pull(&a->bridge, s, n);
}
double recomp_audio_fill_ms(const recomp_audio *a) { return a ? rab_fill_ms(&a->bridge) : 0; }
void recomp_audio_stats(const recomp_audio *a, rab_stats *s) {
    if (s) { if (a) rab_get_stats(&a->bridge, s); else memset(s, 0, sizeof(*s)); }
}
