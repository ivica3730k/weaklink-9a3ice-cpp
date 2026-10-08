#include "weaklink/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>

namespace weaklink {
namespace {

struct State {
  std::mutex mutex;
  std::unique_ptr<std::ofstream> file;
  std::function<void(LogLevel, const std::string&, const std::string&)> sink;
  LogLevel level = LogLevel::kError;
  bool active = false;
};

State& state() {
  static State instance;
  return instance;
}

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarning:
      return "WARNING";
    case LogLevel::kError:
      return "ERROR";
  }
  return "INFO";
}

std::string timestamp() {
  using Clock = std::chrono::system_clock;
  const auto now = Clock::now();
  const auto seconds = Clock::to_time_t(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) %
                      1000;
  std::tm tm_value{};
#if defined(_WIN32)
  localtime_s(&tm_value, &seconds);
#else
  localtime_r(&seconds, &tm_value);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_value);
  char out[48];
  std::snprintf(out, sizeof(out), "%s,%03d", buffer, static_cast<int>(millis.count()));
  return out;
}

}  // namespace

void Logger::configure_file(const std::string& path, LogLevel level) {
  State& s = state();
  std::lock_guard<std::mutex> guard(s.mutex);
  s.file = std::make_unique<std::ofstream>(path, std::ios::app);
  s.sink = nullptr;
  s.level = level;
  s.active = s.file->good();
}

void Logger::configure_sink(
    std::function<void(LogLevel, const std::string&, const std::string&)> sink,
    LogLevel level) {
  State& s = state();
  std::lock_guard<std::mutex> guard(s.mutex);
  s.file.reset();
  s.sink = std::move(sink);
  s.level = level;
  s.active = static_cast<bool>(s.sink);
}

void Logger::silence() {
  State& s = state();
  std::lock_guard<std::mutex> guard(s.mutex);
  s.file.reset();
  s.sink = nullptr;
  s.active = false;
}

bool Logger::enabled(LogLevel level) {
  State& s = state();
  return s.active && static_cast<int>(level) >= static_cast<int>(s.level);
}

void Logger::write(LogLevel level, const std::string& name, const std::string& message) {
  State& s = state();
  std::lock_guard<std::mutex> guard(s.mutex);
  if (!s.active) {
    return;
  }
  if (s.sink) {
    s.sink(level, name, message);
    return;
  }
  if (s.file) {
    *s.file << timestamp() << ' ' << level_name(level) << ' ' << name << ": " << message
            << '\n';
    s.file->flush();
  }
}

}  // namespace weaklink
