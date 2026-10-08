#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <utility>

namespace weaklink {

enum class LogLevel { kDebug = 10, kInfo = 20, kWarning = 30, kError = 40 };

/// Named logger mirroring the Python side's ``weaklink.*`` hierarchy
/// (``weaklink.decode``, ``weaklink.streaming``, ...). Records are formatted
/// as ``<timestamp> <LEVEL> <name>: <message>``.
///
/// Diagnostics never touch stdout/stderr: stdout is the modem's byte pipe.
class Logger {
 public:
  explicit Logger(std::string name) : name_(std::move(name)) {}

  /// Open ``path`` in append mode and set the global threshold. Replaces any
  /// previously configured sink.
  static void configure_file(const std::string& path, LogLevel level);

  /// Route records to a caller-supplied sink instead of a file. Mirrors the
  /// Python API's ``logger=`` kwarg.
  static void configure_sink(std::function<void(LogLevel, const std::string&, const std::string&)> sink,
                             LogLevel level);

  /// Drop every record. The default before ``configure_*`` is called.
  static void silence();

  static bool enabled(LogLevel level);

  template <typename... Args>
  void debug(Args&&... args) const {
    emit(LogLevel::kDebug, std::forward<Args>(args)...);
  }
  template <typename... Args>
  void info(Args&&... args) const {
    emit(LogLevel::kInfo, std::forward<Args>(args)...);
  }
  template <typename... Args>
  void warning(Args&&... args) const {
    emit(LogLevel::kWarning, std::forward<Args>(args)...);
  }
  template <typename... Args>
  void error(Args&&... args) const {
    emit(LogLevel::kError, std::forward<Args>(args)...);
  }

 private:
  template <typename... Args>
  void emit(LogLevel level, Args&&... args) const {
    if (!enabled(level)) {
      return;
    }
    std::ostringstream stream;
    (stream << ... << args);
    write(level, name_, stream.str());
  }

  static void write(LogLevel level, const std::string& name, const std::string& message);

  std::string name_;
};

}  // namespace weaklink
