# SPDX-License-Identifier: MIT AND GPL-3.0-or-later
# Guard patterns below retain fragments of the pinned Chris Sotraidis GPL backend.
# Reuse the complete pinned output pipeline. Keep the donor checkout intact;
# expose host pacing explicitly and retain original PCM cadence (no stretch).
set(_gcnllr_audio_original "${gxruntime}/backends/aurora/aurora_audio.cpp")
file(READ "${_gcnllr_audio_original}" _gcnllr_audio)
string(REPLACE "\r\n" "\n" _gcnllr_audio "${_gcnllr_audio}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gcnllr_audio_original}")
set(_gcnllr_stretch [=[    static const bool enabled = [] {
        const char* env = std::getenv("DOL_AUDIO_STRETCH");
        return env == nullptr || env[0] != '0';
    }();
    return enabled;]=])
set(_gcnllr_throttle [=[    static const bool no_throttle = [] {
        const char* env = std::getenv("DOL_AUDIO_NO_THROTTLE");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();]=])
set(_gcnllr_sample_scan [=[    for (u32 i = 0; i < frames * 2u; i++) {
        const s32 sample = samples[i];
        if (sample != 0)
            nonzero_samples++;
        const s32 magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak_sample)
            peak_sample = magnitude;
        sample_hash ^= static_cast<u16>(sample);
        sample_hash *= 16777619u;
    }]=])
set(_gcnllr_push_sleep [=[    unsigned waited_ms = 0;
    while (queued > max_queued_bytes && waited_ms < 20u) {
        gx_aurora::g_audio_throttle_count++;
        if (gx_aurora::g_audio_queue_log &&
            (gx_aurora::g_audio_throttle_count <= 8 ||
             (gx_aurora::g_audio_throttle_count % 100) == 0))
            std::fprintf(stderr,
                         "[audio-queue] throttle push=%llu waits=%llu queued=%d "
                         "max=%d\n",
                         gx_aurora::g_audio_push_count, gx_aurora::g_audio_throttle_count, queued,
                         max_queued_bytes);
        SDL_Delay(1);
        waited_ms++;
        queued = SDL_GetAudioStreamQueued(gx_aurora::g_audio_stream);
    }]=])
foreach(_gcnllr_expected IN ITEMS _gcnllr_stretch _gcnllr_throttle _gcnllr_sample_scan _gcnllr_push_sleep)
  string(FIND "${_gcnllr_audio}" "${${_gcnllr_expected}}" _gcnllr_found)
  if(_gcnllr_found EQUAL -1)
    message(FATAL_ERROR "Pinned audio adaptation differs: ${_gcnllr_expected}")
  endif()
endforeach()
string(REPLACE "${_gcnllr_stretch}" "    return false;" _gcnllr_audio "${_gcnllr_audio}")
string(REPLACE "${_gcnllr_throttle}" "    const bool no_throttle = recomp_audio_aurora_unpaced();" _gcnllr_audio "${_gcnllr_audio}")
string(REPLACE "${_gcnllr_push_sleep}" "" _gcnllr_audio "${_gcnllr_audio}")
# The caller only consumes the PCM scan in the opt-in queue diagnostics.
string(REPLACE "\n" "\n    " _gcnllr_logged_scan "${_gcnllr_sample_scan}")
string(REPLACE "${_gcnllr_sample_scan}"
  "    if (gx_aurora::g_audio_queue_log) {
    ${_gcnllr_logged_scan}
    }" _gcnllr_audio "${_gcnllr_audio}")
set(_gcnllr_stream "    g_audio_playing = false;\n    if (!g_audio_stream) {")
string(FIND "${_gcnllr_audio}" "${_gcnllr_stream}" _gcnllr_stream_found)
if(_gcnllr_stream_found EQUAL -1)
  message(FATAL_ERROR "Pinned audio stream-open observation differs")
endif()
string(REPLACE "${_gcnllr_stream}"
  "    g_audio_playing = false;\n    if (g_audio_stream && !recomp_audio_aurora_observe_stream(g_audio_stream)) {\n        SDL_DestroyAudioStream(g_audio_stream);\n        g_audio_stream = nullptr;\n    }\n    if (!g_audio_stream) {"
  _gcnllr_audio "${_gcnllr_audio}")
string(REPLACE "#include \"aurora_backend_private.h\"" "#include \"recomp_audio_aurora_private.hpp\""
  _gcnllr_audio "${_gcnllr_audio}")
string(PREPEND _gcnllr_audio "#include \"recomp_audio_aurora.h\"\nusing u16=uint16_t; using u32=uint32_t; using u64=uint64_t; using s16=int16_t; using s32=int32_t;\n")
set(RECOMP_AUDIO_ADAPTED_AURORA "${CMAKE_CURRENT_BINARY_DIR}/recomp-audio-aurora/aurora_audio.cpp")
file(CONFIGURE OUTPUT "${RECOMP_AUDIO_ADAPTED_AURORA}" CONTENT "${_gcnllr_audio}" @ONLY)
