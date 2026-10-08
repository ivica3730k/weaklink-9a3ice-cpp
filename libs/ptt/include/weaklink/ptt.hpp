#pragma once

#include <string>
#include <utility>

namespace weaklink::ptt {

/// Parse ``host``, ``host:port`` or ``:port``. A bare host keeps the default
/// rigctld port; a bare ``:port`` keeps localhost.
std::pair<std::string, int> parse_endpoint(const std::string& spec);

/// Keys the radio on construction and releases it on destruction.
///
/// An empty spec makes the whole thing a no-op, so callers can construct one
/// unconditionally instead of branching around PTT.
class HamlibPtt {
 public:
  explicit HamlibPtt(const std::string& spec);
  ~HamlibPtt();

  HamlibPtt(const HamlibPtt&) = delete;
  HamlibPtt& operator=(const HamlibPtt&) = delete;

 private:
  int socket_ = -1;
};

}  // namespace weaklink::ptt
