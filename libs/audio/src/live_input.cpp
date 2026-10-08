#include <atomic>
#include <thread>
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

std::vector<std::string> parec_command(const std::string& source, int sample_rate) {
  return {"parec",
          "--device=" + source,
          "--format=float32le",
          "--rate=" + std::to_string(sample_rate),
          "--channels=1",
          "--raw",
          "--latency-msec=100"};
}

}  // namespace

struct LiveInputStream::Impl {
  int sample_rate;
  Callback callback;
  AudioTarget target;

  std::atomic<bool> stopping{false};
  std::unique_ptr<process::Pipe> pipe;
  std::thread reader;
  bool started = false;
#if WEAKLINK_HAVE_PORTAUDIO
  std::unique_ptr<detail::PortAudioSession> session;
  PaStream* stream = nullptr;
#endif

  Impl(int rate, Callback cb, AudioTarget tgt)
      : sample_rate(rate), callback(std::move(cb)), target(std::move(tgt)) {}

  void open_parec() {
    pipe = std::make_unique<process::Pipe>(parec_command(target.pulse_name, sample_rate),
                                           process::Pipe::Direction::kReadFromChild);
    if (!pipe->ok()) {
      throw ConfigError("could not start parec for source " + target.pulse_name);
    }
    reader = std::thread([this] {
      // ~50 ms chunks: small enough that the poll loop sees fresh audio every
      // cycle, large enough not to syscall per sample.
      const std::size_t frames = std::max<std::size_t>(1, static_cast<std::size_t>(sample_rate) / 20);
      std::vector<float> buffer(frames);
      while (!stopping.load(std::memory_order_relaxed)) {
        const std::size_t bytes = pipe->read(buffer.data(), buffer.size() * sizeof(float));
        if (bytes == 0) {
          break;
        }
        callback(buffer.data(), bytes / sizeof(float));
      }
    });
  }

#if WEAKLINK_HAVE_PORTAUDIO
  static int portaudio_callback(const void* input, void* /*output*/, unsigned long frames,
                                const PaStreamCallbackTimeInfo* /*timing*/,
                                PaStreamCallbackFlags /*flags*/, void* user_data) {
    auto* self = static_cast<Impl*>(user_data);
    if (input != nullptr) {
      self->callback(static_cast<const float*>(input), static_cast<std::size_t>(frames));
    }
    return paContinue;
  }

  void open_portaudio() {
    session = std::make_unique<detail::PortAudioSession>();
    const int index = target.has_device_index()
                          ? target.device_index
                          : detail::default_device_index(DeviceKind::kInput);
    if (index < 0) {
      throw ConfigError("no audio input device available");
    }
    const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
    if (info == nullptr) {
      throw ConfigError("audio input device " + std::to_string(index) + " does not exist");
    }

    PaStreamParameters parameters{};
    parameters.device = index;
    parameters.channelCount = 1;
    parameters.sampleFormat = detail::kSampleFormatFloat32;
    parameters.suggestedLatency = info->defaultLowInputLatency;
    parameters.hostApiSpecificStreamInfo = nullptr;

    detail::check(Pa_OpenStream(&stream, &parameters, nullptr, sample_rate,
                                detail::kFramesPerBufferUnspecified, detail::kNoStreamFlags, &portaudio_callback, this),
                  "Pa_OpenStream");
    detail::check(Pa_StartStream(stream), "Pa_StartStream");
  }
#endif
};

LiveInputStream::LiveInputStream(int sample_rate, Callback callback, AudioTarget target)
    : impl_(std::make_unique<Impl>(sample_rate, std::move(callback), std::move(target))) {}

LiveInputStream::~LiveInputStream() { stop(); }

void LiveInputStream::start() {
  if (impl_->started) {
    return;
  }
  set_unity_gain(impl_->target, DeviceKind::kInput);
  if (impl_->target.has_pulse_name()) {
    impl_->open_parec();
  } else {
#if WEAKLINK_HAVE_PORTAUDIO
    impl_->open_portaudio();
#else
    throw ConfigError(
        "this build has no live audio support; rebuild with -DWEAKLINK_LIVE_AUDIO=ON "
        "or use --modem-wav");
#endif
  }
  impl_->started = true;
  log().debug("live rx: source ", impl_->target.describe());
}

void LiveInputStream::stop() {
  if (!impl_ || !impl_->started) {
    return;
  }
  impl_->started = false;
  impl_->stopping.store(true, std::memory_order_relaxed);

#if WEAKLINK_HAVE_PORTAUDIO
  if (impl_->stream != nullptr) {
    Pa_AbortStream(impl_->stream);
    Pa_CloseStream(impl_->stream);
    impl_->stream = nullptr;
  }
  impl_->session.reset();
#endif

  if (impl_->pipe) {
    // Kill rather than wait: the reader thread is parked in a blocking read
    // and only returns once the descriptor closes.
    impl_->pipe->kill();
  }
  if (impl_->reader.joinable()) {
    impl_->reader.join();
  }
  impl_->pipe.reset();
}

}  // namespace weaklink::audio
