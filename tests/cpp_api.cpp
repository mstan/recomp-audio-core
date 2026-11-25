#include "recomp_audio.h"
int main() {
    rab_config c;
    rab_config_defaults(&c);
    c.delivery_profile = RECOMP_AUDIO_LEGACY_NES;
    recomp_audio *a = recomp_audio_create(&c);
    if (!a) return 1;
    recomp_audio_reset(a);
    recomp_audio_destroy(a);
    return 0;
}
