#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
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

/// Resolve the device hint the same way for every entry point, including the
/// ``PULSE_SINK`` fallback.
AudioTarget resolve_output(const std::string& device) {
  std::string hint = device;
  if (hint.empty()) {
    const char* environment = std::getenv("PULSE_SINK");
    if (environment != nullptr) {
      hint = environment;
    }
  }
  const AudioTarget target = resolve_audio_target(hint, DeviceKind::kOutput);
  set_unity_gain(target, DeviceKind::kOutput);
  return target;
}

}  // namespace

struct PlaybackStream::Impl {
  std::unique_ptr<process::Pipe> pipe;
  bool closed = false;
#if WEAKLINK_HAVE_PORTAUDIO
  std::unique_ptr<detail::PortAudioSession> session;
  PaStream* stream = nullptr;
#endif
};

PlaybackStream::PlaybackStream(double sample_rate, const std::string& device)
    : impl_(std::make_unique<Impl>()) {
  const AudioTarget target = resolve_output(device);
  const auto rate = static_cast<int>(std::lround(sample_rate));

  if (target.has_pulse_name()) {
    impl_->pipe = std::make_unique<process::Pipe>(paplay_command(target.pulse_name, rate),
                                                  process::Pipe::Direction::kWriteToChild);
    if (!impl_->pipe->ok()) {
      throw ConfigError("could not start paplay for sink " + target.pulse_name);
    }
    log().debug("live tx: ", target.describe(), " at ", rate, " Hz");
    return;
  }

#if WEAKLINK_HAVE_PORTAUDIO
  impl_->session = std::make_unique<detail::PortAudioSession>();

  const int index = target.has_device_index()
                        ? target.device_index
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

  detail::check(Pa_OpenStream(&impl_->stream, nullptr, &parameters, rate,
                              detail::kFramesPerBufferUnspecified, detail::kNoStreamFlags,
                              nullptr, nullptr),
                "Pa_OpenStream");
  detail::check(Pa_StartStream(impl_->stream), "Pa_StartStream");
  log().debug("live tx: ", target.describe(), " at ", rate, " Hz");
#else
  throw ConfigError(
      "this build has no live audio support; rebuild with -DWEAKLINK_LIVE_AUDIO=ON "
      "or use --modem-wav");
#endif
}

PlaybackStream::~PlaybackStream() {
  try {
    close();
  } catch (...) {
    // A destructor must not throw; the audio has already gone out or not.
  }
}

void PlaybackStream::write(const float* samples, std::size_t count) {
  if (impl_->closed || count == 0) {
    return;
  }
  if (impl_->pipe) {
    // A broken pipe means the sink went away; stop feeding rather than
    // spinning on a dead child.
    if (!impl_->pipe->write(samples, count * sizeof(float))) {
      impl_->closed = true;
    }
    return;
  }
#if WEAKLINK_HAVE_PORTAUDIO
  const PaError status =
      Pa_WriteStream(impl_->stream, samples, static_cast<unsigned long>(count));
  // An underflow means the sink starved for a moment; the audio is still going
  // out, so it is not worth aborting the transmission over.
  if (status != paNoError && status != paOutputUnderflowed) {
    detail::check(status, "Pa_WriteStream");
  }
#endif
}

void PlaybackStream::close() {
  if (!impl_ || impl_->closed) {
    return;
  }
  impl_->closed = true;

  if (impl_->pipe) {
    const int status = impl_->pipe->finish();
    impl_->pipe.reset();
    if (status != 0) {
      throw ConfigError("paplay exited " + std::to_string(status));
    }
    return;
  }
#if WEAKLINK_HAVE_PORTAUDIO
  if (impl_->stream != nullptr) {
    Pa_StopStream(impl_->stream);
    Pa_CloseStream(impl_->stream);
    impl_->stream = nullptr;
  }
  impl_->session.reset();
#endif
}

void PlaybackStream::abort() {
  if (!impl_ || impl_->closed) {
    return;
  }
  impl_->closed = true;

  if (impl_->pipe) {
    // paplay buffers ahead of the speaker, so asking it to exit cleanly would
    // still let the tail play; kill it instead.
    impl_->pipe->kill();
    impl_->pipe.reset();
    return;
  }
#if WEAKLINK_HAVE_PORTAUDIO
  if (impl_->stream != nullptr) {
    Pa_AbortStream(impl_->stream);
    Pa_CloseStream(impl_->stream);
    impl_->stream = nullptr;
  }
  impl_->session.reset();
#endif
}

void play_stream(const SampleSource& source, double sample_rate, const std::string& device) {
  PlaybackStream stream(sample_rate, device);
  Samples chunk;
  while (true) {
    chunk.clear();
    if (!source(chunk)) {
      break;
    }
    stream.write(chunk);
  }
  stream.close();
}

void play(const Samples& samples, double sample_rate, const std::string& device) {
  PlaybackStream stream(sample_rate, device);
  stream.write(samples);
  stream.close();
}

}  // namespace weaklink::audio
