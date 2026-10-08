#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "weaklink/wav.hpp"

namespace weaklink::audio {

enum class DeviceKind { kInput, kOutput };

/// A resolved audio endpoint. At most one of the two fields is set; both unset
/// means "let the OS pick its default".
struct AudioTarget {
  int device_index = -1;        ///< PortAudio device index, or -1.
  std::string pulse_name;       ///< PulseAudio sink/source name, or empty.

  bool has_device_index() const { return device_index >= 0; }
  bool has_pulse_name() const { return !pulse_name.empty(); }
  std::string describe() const;
};

/// Turn a user-supplied device hint into a concrete backend target.
///
/// Four forms are accepted, in priority order:
///  1. ``pulse:<name|id>``  force the Pulse path (ids resolved via ``pactl``).
///  2. a bare integer       a Pulse id if ``pactl`` knows it, else a PortAudio
///                          device index -- so a Linux user can paste an id
///                          from ``pactl list short sinks`` without a prefix,
///                          while macOS and Windows keep index-by-int.
///  3. a name substring     matched against PortAudio device names.
///  4. a Pulse sink/source  driven through the ``paplay`` / ``parec``
///                          subprocess, which is the only way to reach named
///                          endpoints that PortAudio's Pulse compat layer
///                          hides (``virt.monitor`` and friends).
AudioTarget resolve_audio_target(const std::string& hint, DeviceKind kind);

/// Best-effort unity gain (100% / 0 dB) and unmute before the stream opens.
/// Failures are logged and swallowed: the modem still runs at whatever gain
/// the OS had set.
void set_unity_gain(const AudioTarget& target, DeviceKind kind);

/// Pulls the next chunk of audio to play. Return false when the stream ends.
using SampleSource = std::function<bool(Samples&)>;

/// Play float32 mono audio as it arrives, so a long transmission never has to
/// be buffered in full. Pulse targets pipe into ``paplay --raw``; everything
/// else goes through a PortAudio output stream.
void play_stream(const SampleSource& source, double sample_rate, const std::string& device);

/// Blocking one-shot play.
void play(const Samples& samples, double sample_rate, const std::string& device);

/// Uniform live-audio input over PortAudio or ``parec``.
///
/// Chunks are delivered on a producer thread; the caller polls between sleeps
/// rather than doing work in the callback.
class LiveInputStream {
 public:
  using Callback = std::function<void(const float*, std::size_t)>;

  LiveInputStream(int sample_rate, Callback callback, AudioTarget target);
  ~LiveInputStream();

  LiveInputStream(const LiveInputStream&) = delete;
  LiveInputStream& operator=(const LiveInputStream&) = delete;

  void start();
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// True when the build includes PortAudio. When false, live device I/O raises
/// ``ConfigError`` and only WAV and stdin/stdout sample modes work.
bool live_audio_available();

}  // namespace weaklink::audio
