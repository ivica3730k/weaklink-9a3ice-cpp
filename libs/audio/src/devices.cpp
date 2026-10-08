#include <algorithm>
#include <cctype>
#include <sstream>

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

bool is_integer(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  std::size_t start = text[0] == '-' ? 1 : 0;
  if (start >= text.size()) {
    return false;
  }
  return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(start), text.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string to_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

/// Resolve a numeric Pulse sink/source index to its name via
/// ``pactl list short``. Returns an empty string when pactl is missing, fails,
/// or has no matching row.
std::string pactl_lookup_id(const std::string& id, DeviceKind kind) {
  if (!process::available("pactl")) {
    return {};
  }
  const char* subcommand = kind == DeviceKind::kInput ? "sources" : "sinks";
  std::string output;
  if (!process::run_capture({"pactl", "list", "short", subcommand}, output)) {
    log().warning("pactl ", subcommand, " failed for id ", id);
    return {};
  }
  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    if (line.substr(0, tab) == id) {
      const std::size_t next = line.find('\t', tab + 1);
      return line.substr(tab + 1, next == std::string::npos ? std::string::npos : next - tab - 1);
    }
  }
  return {};
}

/// ``pulse:<name>`` or ``pulse:<id>``. A numeric reference that pactl cannot
/// resolve is passed through raw rather than dropped -- the user asked for the
/// Pulse path explicitly.
AudioTarget resolve_pulse(const std::string& reference, DeviceKind kind) {
  if (reference.empty()) {
    return {};
  }
  if (is_integer(reference)) {
    const std::string resolved = pactl_lookup_id(reference, kind);
    if (!resolved.empty()) {
      log().debug("pulse:", reference, " -> ", resolved, " (via pactl)");
      AudioTarget target;
      target.pulse_name = resolved;
      return target;
    }
    log().warning("no Pulse endpoint at index ", reference, "; passing raw");
  }
  AudioTarget target;
  target.pulse_name = reference;
  return target;
}

void pactl_set_unity(const std::string& endpoint, DeviceKind kind) {
  if (!process::available("pactl")) {
    log().debug("pactl not on PATH; skipping unity-gain set for ", endpoint);
    return;
  }
  const char* volume_command =
      kind == DeviceKind::kInput ? "set-source-volume" : "set-sink-volume";
  const char* mute_command = kind == DeviceKind::kInput ? "set-source-mute" : "set-sink-mute";
  if (process::run_quiet({"pactl", volume_command, endpoint, "100%"}) != 0) {
    log().debug("pactl ", volume_command, " failed for ", endpoint);
    return;
  }
  if (process::run_quiet({"pactl", mute_command, endpoint, "0"}) != 0) {
    log().debug("pactl ", mute_command, " failed for ", endpoint);
    return;
  }
  log().debug("pactl: ", endpoint, " set to unity + unmuted");
}

void osascript_set_unity(DeviceKind kind) {
  if (!process::available("osascript")) {
    log().debug("osascript not on PATH; skipping unity-gain set");
    return;
  }
  // Input needs only the volume set (100 unmutes implicitly); output has a
  // separate muted flag.
  std::vector<std::string> scripts;
  if (kind == DeviceKind::kInput) {
    scripts = {"set volume input volume 100"};
  } else {
    scripts = {"set volume output volume 100", "set volume output muted false"};
  }
  for (const std::string& script : scripts) {
    if (process::run_quiet({"osascript", "-e", script}) != 0) {
      log().debug("osascript ", script, " failed");
      return;
    }
  }
  log().debug("osascript: system volume set to unity + unmuted");
}

}  // namespace

std::string AudioTarget::describe() const {
  if (has_pulse_name()) {
    return "pulse:" + pulse_name;
  }
  if (has_device_index()) {
    return "portaudio[" + std::to_string(device_index) + "]";
  }
  return "default";
}

bool live_audio_available() {
#if WEAKLINK_HAVE_PORTAUDIO
  return true;
#else
  return false;
#endif
}

AudioTarget resolve_audio_target(const std::string& hint, DeviceKind kind) {
  if (hint.empty()) {
    return {};
  }

  constexpr const char* kPulsePrefix = "pulse:";
  if (hint.rfind(kPulsePrefix, 0) == 0) {
    return resolve_pulse(hint.substr(std::char_traits<char>::length(kPulsePrefix)), kind);
  }

  // A bare integer: try Pulse first so a Linux user can paste an id straight
  // from `pactl list short sinks`. Without pactl (macOS, Windows) it stays a
  // PortAudio device index.
  if (is_integer(hint)) {
    const std::string resolved = pactl_lookup_id(hint, kind);
    if (!resolved.empty()) {
      log().debug("hint ", hint, " -> pulse:", resolved, " (via pactl)");
      AudioTarget target;
      target.pulse_name = resolved;
      return target;
    }
    AudioTarget target;
    target.device_index = std::stoi(hint);
    return target;
  }

#if WEAKLINK_HAVE_PORTAUDIO
  {
    detail::PortAudioSession session;
    const std::string wanted = to_lower(hint);
    const int count = Pa_GetDeviceCount();
    for (int index = 0; index < count; ++index) {
      const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
      if (info == nullptr) {
        continue;
      }
      const int channels =
          kind == DeviceKind::kInput ? info->maxInputChannels : info->maxOutputChannels;
      if (channels <= 0) {
        continue;
      }
      const std::string name = to_lower(info->name == nullptr ? "" : info->name);
      // Skip the abstract Pulse/PipeWire compat devices so the subprocess path
      // can claim named Pulse endpoints.
      if (name == "pulse" || name == "pipewire" || name == "default") {
        continue;
      }
      if (name.find(wanted) != std::string::npos || wanted.find(name) != std::string::npos) {
        log().debug("device hint ", hint, " -> portaudio ", index, " ", name);
        AudioTarget target;
        target.device_index = index;
        return target;
      }
    }
  }
#endif

  const char* tool = kind == DeviceKind::kInput ? "parec" : "paplay";
  if (process::available(tool)) {
    log().debug("device hint ", hint, " -> pulse subprocess (", tool, " --device=", hint, ")");
    AudioTarget target;
    target.pulse_name = hint;
    return target;
  }

  log().warning("device hint ", hint, " did not match any audio device and ", tool,
                " is not on PATH; using OS default");
  return {};
}

void set_unity_gain(const AudioTarget& target, DeviceKind kind) {
  if (target.has_pulse_name()) {
    pactl_set_unity(target.pulse_name, kind);
    return;
  }
#if defined(__APPLE__)
  (void)kind;
  osascript_set_unity(kind);
#elif defined(__linux__)
  // PortAudio's Pulse compatibility routes to the server default, so that is
  // the endpoint worth pinning when no explicit Pulse name was given.
  pactl_set_unity(kind == DeviceKind::kInput ? "@DEFAULT_SOURCE@" : "@DEFAULT_SINK@", kind);
#else
  (void)kind;
#endif
}

namespace detail {

#if WEAKLINK_HAVE_PORTAUDIO

PortAudioSession::PortAudioSession() { check(Pa_Initialize(), "Pa_Initialize"); }

PortAudioSession::~PortAudioSession() { Pa_Terminate(); }

void check(PaError code, const std::string& context) {
  if (code != paNoError) {
    throw ConfigError(context + " failed: " + Pa_GetErrorText(code));
  }
}

int default_device_index(DeviceKind kind) {
  return kind == DeviceKind::kInput ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
}

#else

int default_device_index(DeviceKind) { return -1; }

#endif

}  // namespace detail
}  // namespace weaklink::audio
