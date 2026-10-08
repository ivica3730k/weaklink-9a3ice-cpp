#pragma once

#include <string>

#include "weaklink/audio.hpp"

#if WEAKLINK_HAVE_PORTAUDIO
#include <portaudio.h>
#endif

namespace weaklink::audio::detail {

#if WEAKLINK_HAVE_PORTAUDIO

/// Reference-counted ``Pa_Initialize`` / ``Pa_Terminate``. PortAudio is a
/// process-global library and re-initialising it mid-stream is not safe, so
/// every entry point holds one of these for as long as it needs the library.
class PortAudioSession {
 public:
  PortAudioSession();
  ~PortAudioSession();

  PortAudioSession(const PortAudioSession&) = delete;
  PortAudioSession& operator=(const PortAudioSession&) = delete;
};

/// Throws ``ConfigError`` carrying PortAudio's own message when ``code`` is an
/// error.
void check(PaError code, const std::string& context);

// PortAudio's public constants are macros wrapping C-style casts. Giving them
// typed names once here keeps the casts out of every call site.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
inline const PaSampleFormat kSampleFormatFloat32 = paFloat32;
inline const PaStreamFlags kNoStreamFlags = paNoFlag;
inline const unsigned long kFramesPerBufferUnspecified = paFramesPerBufferUnspecified;
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif

/// Resolve a PortAudio device index to use, or ``paNoDevice``-equivalent -1 to
/// mean "the host API default".
int default_device_index(DeviceKind kind);

}  // namespace weaklink::audio::detail
