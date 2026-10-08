#include "weaklink/constants.hpp"

namespace weaklink {

const std::map<double, BaudPreset>& baud_presets() {
  static const std::map<double, BaudPreset> presets = {
      {45.0, BaudPreset{200.0, 16, 8, 4, 4}},
      {300.0, BaudPreset{300.0, 16, 8, 2, 4}},
      {1200.0, BaudPreset{1200.0, 16, 8, 2, 4}},
  };
  return presets;
}

}  // namespace weaklink
