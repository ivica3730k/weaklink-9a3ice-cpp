#pragma once

#include <string>
#include <vector>

namespace weaklink::audio::process {

/// True when ``name`` is an executable on PATH. Mirrors ``shutil.which``.
bool available(const std::string& name);

/// Run a command to completion and capture stdout. Returns false if the
/// command could not be started or exited non-zero.
bool run_capture(const std::vector<std::string>& argv, std::string& stdout_text);

/// Run a command to completion, discarding output. Returns the exit status, or
/// -1 if the process could not be started.
int run_quiet(const std::vector<std::string>& argv);

/// A child process with one pipe attached, used for the ``paplay`` / ``parec``
/// endpoints that PortAudio's Pulse compatibility layer cannot reach.
class Pipe {
 public:
  enum class Direction { kWriteToChild, kReadFromChild };

  Pipe(const std::vector<std::string>& argv, Direction direction);
  ~Pipe();

  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  bool ok() const { return pid_ > 0; }

  /// Returns false once the child has closed its end of the pipe.
  bool write(const void* data, std::size_t bytes);

  /// Returns the number of bytes read; 0 means EOF.
  std::size_t read(void* data, std::size_t bytes);

  /// Close our end of the pipe. The child sees EOF and exits.
  void close_pipe();

  /// Close the pipe and wait for the child. Returns its exit status.
  int finish();

  /// Terminate immediately without waiting -- the OS reaps the pipes and any
  /// blocked reader returns as soon as the descriptor closes.
  void kill();

 private:
  int fd_ = -1;
  long pid_ = -1;
  bool finished_ = false;
  int status_ = 0;
};

}  // namespace weaklink::audio::process
