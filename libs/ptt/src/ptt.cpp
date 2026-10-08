#include "weaklink/ptt.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "weaklink/constants.hpp"
#include "weaklink/exceptions.hpp"
#include "weaklink/log.hpp"

namespace weaklink::ptt {
namespace {

Logger& log() {
  static Logger logger("weaklink.ptt");
  return logger;
}

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void close_socket(int handle) { ::closesocket(static_cast<SocketHandle>(handle)); }

/// Winsock needs process-wide initialisation before any socket call.
struct WinsockSession {
  WinsockSession() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WinsockSession() { WSACleanup(); }
};
#else
void close_socket(int handle) { ::close(handle); }
#endif

int connect_to(const std::string& host, int port) {
#if defined(_WIN32)
  static WinsockSession winsock;
#endif
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
    throw PTTError("rigctld connect " + host + ":" + service + " failed: host not found");
  }

  int handle = -1;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    const int candidate =
        static_cast<int>(::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol));
    if (candidate < 0) {
      continue;
    }
    if (::connect(candidate, entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen)) == 0) {
      handle = candidate;
      break;
    }
    close_socket(candidate);
  }
  ::freeaddrinfo(results);

  if (handle < 0) {
    throw PTTError("rigctld connect " + host + ":" + service + " failed: " +
                   std::strerror(errno));
  }
  return handle;
}

void send_command(int handle, const char* command) {
  const std::size_t length = std::strlen(command);
  std::size_t sent = 0;
  while (sent < length) {
#if defined(_WIN32)
    const int written = ::send(static_cast<SocketHandle>(handle), command + sent,
                               static_cast<int>(length - sent), 0);
#else
    const ssize_t written = ::send(handle, command + sent, length - sent, 0);
#endif
    if (written <= 0) {
      throw PTTError(std::string("rigctld ") + command + " failed: " + std::strerror(errno));
    }
    sent += static_cast<std::size_t>(written);
  }
}

void sleep_seconds(double seconds) {
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

}  // namespace

std::pair<std::string, int> parse_endpoint(const std::string& spec) {
  const std::size_t colon = spec.find(':');
  if (colon == std::string::npos) {
    return {spec.empty() ? "localhost" : spec, kHamlibDefaultPort};
  }
  std::string host = spec.substr(0, colon);
  const std::string port_text = spec.substr(colon + 1);
  if (host.empty()) {
    host = "localhost";
  }
  if (port_text.empty()) {
    return {host, kHamlibDefaultPort};
  }
  try {
    return {host, std::stoi(port_text)};
  } catch (const std::exception&) {
    throw ConfigError("invalid --hamlib-ptt port '" + port_text + "'");
  }
}

HamlibPtt::HamlibPtt(const std::string& spec) {
  if (spec.empty()) {
    return;
  }
  const auto endpoint = parse_endpoint(spec);
  log().debug("hamlib PTT: connecting to ", endpoint.first, ":", endpoint.second);
  socket_ = connect_to(endpoint.first, endpoint.second);
  try {
    send_command(socket_, "T 1\n");
  } catch (...) {
    close_socket(socket_);
    socket_ = -1;
    throw;
  }
  // Radios need a gap between key-up and the first sample or the leading pilot
  // is clipped by relay and AGC settling.
  log().debug("hamlib PTT: keyed, waiting ", kHamlibPttLeadSeconds * 1000, " ms");
  sleep_seconds(kHamlibPttLeadSeconds);
}

HamlibPtt::~HamlibPtt() {
  if (socket_ < 0) {
    return;
  }
  // Symmetric tail: hold past the last sample so the trailing pilot makes it
  // out before the relay drops.
  log().debug("hamlib PTT: holding tail ", kHamlibPttTailSeconds * 1000, " ms");
  sleep_seconds(kHamlibPttTailSeconds);
  try {
    send_command(socket_, "T 0\n");
    log().debug("hamlib PTT: released");
  } catch (const std::exception& error) {
    log().warning("hamlib PTT: release failed: ", error.what());
  }
  close_socket(socket_);
  socket_ = -1;
}

}  // namespace weaklink::ptt
