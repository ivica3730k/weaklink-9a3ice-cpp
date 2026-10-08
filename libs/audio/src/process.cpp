#include "process.hpp"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <array>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace weaklink::audio::process {

#if defined(_WIN32)

// The Pulse subprocess path has no Windows equivalent. Everything here reports
// "not available" so callers fall back to PortAudio or raise a clean error.

bool available(const std::string&) { return false; }
bool run_capture(const std::vector<std::string>&, std::string&) { return false; }
int run_quiet(const std::vector<std::string>&) { return -1; }

Pipe::Pipe(const std::vector<std::string>&, Direction) {}
Pipe::~Pipe() = default;
bool Pipe::write(const void*, std::size_t) { return false; }
std::size_t Pipe::read(void*, std::size_t) { return 0; }
void Pipe::close_pipe() {}
int Pipe::finish() { return -1; }
void Pipe::kill() {}

#else

namespace {

/// argv for execvp: a NUL-terminated array of mutable C strings.
class ArgvBuffer {
 public:
  explicit ArgvBuffer(const std::vector<std::string>& argv) {
    storage_.reserve(argv.size());
    pointers_.reserve(argv.size() + 1);
    for (const std::string& argument : argv) {
      storage_.emplace_back(argument.begin(), argument.end());
      storage_.back().push_back('\0');
      pointers_.push_back(storage_.back().data());
    }
    pointers_.push_back(nullptr);
  }

  char* const* data() { return pointers_.data(); }

 private:
  std::vector<std::vector<char>> storage_;
  std::vector<char*> pointers_;
};

[[noreturn]] void exec_child(const std::vector<std::string>& argv) {
  ArgvBuffer buffer(argv);
  ::execvp(argv.front().c_str(), buffer.data());
  ::_exit(127);
}

}  // namespace

bool available(const std::string& name) {
  const char* path = std::getenv("PATH");
  if (path == nullptr) {
    return false;
  }
  const std::string haystack(path);
  std::size_t start = 0;
  while (start <= haystack.size()) {
    const std::size_t end = haystack.find(':', start);
    const std::string directory =
        haystack.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!directory.empty()) {
      const std::string candidate = directory + "/" + name;
      if (::access(candidate.c_str(), X_OK) == 0) {
        return true;
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

bool run_capture(const std::vector<std::string>& argv, std::string& stdout_text) {
  stdout_text.clear();
  int fds[2];
  if (::pipe(fds) != 0) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return false;
  }
  if (pid == 0) {
    ::close(fds[0]);
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[1]);
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    exec_child(argv);
  }
  ::close(fds[1]);
  char buffer[4096];
  ssize_t count = 0;
  while ((count = ::read(fds[0], buffer, sizeof(buffer))) > 0) {
    stdout_text.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int run_quiet(const std::vector<std::string>& argv) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    exec_child(argv);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

Pipe::Pipe(const std::vector<std::string>& argv, Direction direction) {
  int fds[2];
  if (::pipe(fds) != 0) {
    return;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return;
  }
  if (pid == 0) {
    if (direction == Direction::kWriteToChild) {
      ::close(fds[1]);
      ::dup2(fds[0], STDIN_FILENO);
      ::close(fds[0]);
    } else {
      ::close(fds[0]);
      ::dup2(fds[1], STDOUT_FILENO);
      ::close(fds[1]);
    }
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    exec_child(argv);
  }
  if (direction == Direction::kWriteToChild) {
    ::close(fds[0]);
    fd_ = fds[1];
  } else {
    ::close(fds[1]);
    fd_ = fds[0];
  }
  pid_ = pid;
  // A child that exits early would otherwise kill us with SIGPIPE instead of
  // letting write() report EPIPE.
  ::signal(SIGPIPE, SIG_IGN);
}

Pipe::~Pipe() {
  close_pipe();
  if (pid_ > 0 && !finished_) {
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  }
}

bool Pipe::write(const void* data, std::size_t bytes) {
  if (fd_ < 0) {
    return false;
  }
  const char* cursor = static_cast<const char*>(data);
  std::size_t remaining = bytes;
  while (remaining > 0) {
    const ssize_t written = ::write(fd_, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;  // EPIPE: the sink went away
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

std::size_t Pipe::read(void* data, std::size_t bytes) {
  if (fd_ < 0) {
    return 0;
  }
  char* cursor = static_cast<char*>(data);
  std::size_t filled = 0;
  while (filled < bytes) {
    const ssize_t count = ::read(fd_, cursor + filled, bytes - filled);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (count == 0) {
      break;
    }
    filled += static_cast<std::size_t>(count);
  }
  return filled;
}

void Pipe::close_pipe() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

int Pipe::finish() {
  close_pipe();
  if (pid_ <= 0 || finished_) {
    return status_;
  }
  int status = 0;
  ::waitpid(static_cast<pid_t>(pid_), &status, 0);
  finished_ = true;
  status_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return status_;
}

void Pipe::kill() {
  if (pid_ > 0 && !finished_) {
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
  }
  close_pipe();
}

#endif

}  // namespace weaklink::audio::process
