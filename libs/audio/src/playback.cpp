#include <algorithm>
#include <cmath>
#include <vector>

#include "portaudio_support.hpp"
#include "process.hpp"
#include "weaklink/audio.hpp"
#include "weaklink/exceptions.hpp"
#include "weaklink/log.hpp"

namespace weaklink::audio {
namespace {

Logger& log() {
  static Logger logger("weaklink.audio");
  return logger;
}

std::vector<std::string> paplay_command(const std::string& sink, int sample_rate) {
  return {"paplay",
          "--device=" + sink,
          "--format=float32le",
          "--rate=" + std::to_string(sample_rate),
          "--channels=1",
          "--raw"};
}

void play_pulse_stream(const SampleSource& source, int sample_rate, const std::string& sink) {
  process::Pipe pipe(paplay_command(sink, sample_rate), process::Pipe::Direction::kWriteToChild);
  if (!pipe.ok()) {
    throw ConfigError("could not start paplay for sink " + sink);
  }
  Samples chunk;
  while (true) {
    chunk.clear();
    if (!source(chunk)) {
      break;
    }
    if (chunk.empty()) {
      continue;
    }
    // A broken pipe means the sink went away; stop feeding rather than
    // spinning on a dead child.
    if (!pipe.write(chunk.data(), chunk.size() * sizeof(float))) {
      break;
    }
  }
  const int status = pipe.finish();
  if (status != 0) {
    throw ConfigError("paplay exited " + std::to_string(status));
  }
}

}  // namespace

void play_stream(const SampleSource& source, double sample_rate, const std::string& device) {
  std::string hint = device;
  if (hint.empty()) {
    const char* environment = std::getenv("PULSE_SINK");
    if (environment != nullptr) {
      hint = environment;
    }
  }
  const AudioTarget target = resolve_audio_target(hint, DeviceKind::kOutput);
  set_unity_gain(target, DeviceKind::kOutput);
  const auto rate = static_cast<int>(std::lround(sample_rate));

  if (target.has_pulse_name()) {
    play_pulse_stream(source, rate, target.pulse_name);
    return;
  }

#if WEAKLINK_HAVE_PORTAUDIO
  detail::PortAudioSession session;

  const int index = target.has_device_index() ? target.device_index
                                              : detail::default_device_index(DeviceKind::kOutput);
  if (index < 0) {
    throw ConfigError("no audio output device available");
  }
  const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
  if (info == nullptr) {
    throw ConfigError("audio output device " + std::to_string(index) + " does not exist");
  }

  PaStreamParameters parameters{};
  parameters.device = index;
  parameters.channelCount = 1;
  parameters.sampleFormat = detail::kSampleFormatFloat32;
  parameters.suggestedLatency = info->defaultLowOutputLatency;
  parameters.hostApiSpecificStreamInfo = nullptr;

  PaStream* stream = nullptr;
  detail::check(Pa_OpenStream(&stream, nullptr, &parameters, rate, detail::kFramesPerBufferUnspecified,
                              detail::kNoStreamFlags, nullptr, nullptr),
                "Pa_OpenStream");
  detail::check(Pa_StartStream(stream), "Pa_StartStream");

  log().debug("live tx: ", target.describe(), " at ", rate, " Hz");

  Samples chunk;
  try {
    while (true) {
      chunk.clear();
      if (!source(chunk)) {
        break;
      }
      if (chunk.empty()) {
        continue;
      }
      const PaError status =
          Pa_WriteStream(stream, chunk.data(), static_cast<unsigned long>(chunk.size()));
      // An underflow means the sink starved for a moment; the audio is still
      // going out, so it is not worth aborting the transmission over.
      if (status != paNoError && status != paOutputUnderflowed) {
        detail::check(status, "Pa_WriteStream");
      }
    }
  } catch (...) {
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    throw;
  }
  Pa_StopStream(stream);
  Pa_CloseStream(stream);
#else
  (void)source;
  throw ConfigError(
      "this build has no live audio support; rebuild with -DWEAKLINK_LIVE_AUDIO=ON "
      "or use --modem-wav");
#endif
}

void play(const Samples& samples, double sample_rate, const std::string& device) {
  bool sent = false;
  play_stream(
      [&](Samples& chunk) {
        if (sent) {
          return false;
        }
        chunk = samples;
        sent = true;
        return true;
      },
      sample_rate, device);
}

}  // namespace weaklink::audio
