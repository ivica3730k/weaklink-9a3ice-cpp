#include "weaklink/fec.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace weaklink::fec {
namespace {

constexpr int kGenerators[2] = {0171, 0133};

struct Trellis {
  std::array<std::array<uint8_t, 2>, kNumStates> next_state{};
  std::array<std::array<std::array<int8_t, 2>, 2>, kNumStates> outputs{};
  /// Both predecessors of a destination state share the same input bit (the
  /// bit that becomes the new top bit), so the table stores only the previous
  /// states; the input bit is recovered from the destination during traceback.
  std::array<std::array<uint8_t, 2>, kNumStates> predecessors{};

  Trellis() {
    for (std::size_t state = 0; state < kNumStates; ++state) {
      for (int input_bit = 0; input_bit < 2; ++input_bit) {
        const int combined = (input_bit << (kConstraintLength - 1)) | static_cast<int>(state);
        for (int tap = 0; tap < 2; ++tap) {
          int masked = combined & kGenerators[tap];
          int parity = 0;
          while (masked) {
            parity ^= masked & 1;
            masked >>= 1;
          }
          outputs[state][static_cast<std::size_t>(input_bit)][static_cast<std::size_t>(tap)] =
              static_cast<int8_t>(parity);
        }
        next_state[state][static_cast<std::size_t>(input_bit)] =
            static_cast<uint8_t>(combined >> 1);
      }
    }
    std::array<int, kNumStates> filled{};
    for (std::size_t state = 0; state < kNumStates; ++state) {
      for (int input_bit = 0; input_bit < 2; ++input_bit) {
        const std::size_t destination = next_state[state][static_cast<std::size_t>(input_bit)];
        predecessors[destination][static_cast<std::size_t>(filled[destination]++)] =
            static_cast<uint8_t>(state);
      }
    }
  }
};

const Trellis& trellis() {
  static const Trellis instance;
  return instance;
}

}  // namespace

BitVector encode(const BitVector& bits) {
  const Trellis& t = trellis();
  const std::size_t total = bits.size() + kConstraintLength - 1;
  BitVector out(2 * total);
  std::size_t state = 0;
  for (std::size_t index = 0; index < total; ++index) {
    const std::size_t input_bit = index < bits.size() ? (bits[index] & 1u) : 0u;
    const auto& pair = t.outputs[state][input_bit];
    out[2 * index] = static_cast<uint8_t>(pair[0]);
    out[2 * index + 1] = static_cast<uint8_t>(pair[1]);
    state = t.next_state[state][input_bit];
  }
  return out;
}

BitVector decode(const std::vector<double>& soft_bits, std::size_t num_output_bits) {
  const std::size_t num_steps = num_output_bits + kConstraintLength - 1;
  const std::size_t expected = 2 * num_steps;
  if (soft_bits.size() != expected) {
    throw std::invalid_argument("soft_bits length " + std::to_string(soft_bits.size()) +
                                " != expected " + std::to_string(expected));
  }

  const Trellis& t = trellis();

  // Branch metric signs: an output bit of 0 adds +LLR, a 1 subtracts it.
  std::array<std::array<std::array<double, 2>, 2>, kNumStates> signs{};
  for (std::size_t state = 0; state < kNumStates; ++state) {
    for (std::size_t input_bit = 0; input_bit < 2; ++input_bit) {
      for (std::size_t tap = 0; tap < 2; ++tap) {
        signs[state][input_bit][tap] =
            1.0 - 2.0 * static_cast<double>(t.outputs[state][input_bit][tap]);
      }
    }
  }

  // Destination state's top bit is the input bit that produced it, since
  // dest = ((u << K-1) | prev) >> 1.
  std::array<std::size_t, kNumStates> destination_input_bit{};
  for (std::size_t state = 0; state < kNumStates; ++state) {
    destination_input_bit[state] = (state >> (kConstraintLength - 2)) & 1u;
  }

  constexpr double kNegativeInfinity = -std::numeric_limits<double>::infinity();
  std::array<double, kNumStates> metric{};
  metric.fill(kNegativeInfinity);
  metric[0] = 0.0;

  std::vector<uint8_t> chosen_predecessor(num_steps * kNumStates, 0);
  std::array<double, kNumStates> candidate_zero{};
  std::array<double, kNumStates> candidate_one{};
  std::array<double, kNumStates> updated{};

  for (std::size_t step = 0; step < num_steps; ++step) {
    const double first = soft_bits[2 * step];
    const double second = soft_bits[2 * step + 1];
    for (std::size_t state = 0; state < kNumStates; ++state) {
      candidate_zero[state] =
          metric[state] + signs[state][0][0] * first + signs[state][0][1] * second;
      candidate_one[state] =
          metric[state] + signs[state][1][0] * first + signs[state][1][1] * second;
    }
    for (std::size_t destination = 0; destination < kNumStates; ++destination) {
      const std::size_t input_bit = destination_input_bit[destination];
      const auto& candidates = input_bit == 0 ? candidate_zero : candidate_one;
      const double a = candidates[t.predecessors[destination][0]];
      const double b = candidates[t.predecessors[destination][1]];
      const bool pick_second = b > a;
      updated[destination] = pick_second ? b : a;
      chosen_predecessor[step * kNumStates + destination] = pick_second ? 1u : 0u;
    }
    metric = updated;
  }

  // Traceback from the terminating state 0.
  BitVector output_bits(num_steps, 0);
  std::size_t state = 0;
  for (std::size_t step = num_steps; step-- > 0;) {
    output_bits[step] = static_cast<uint8_t>((state >> (kConstraintLength - 2)) & 1u);
    const uint8_t pick = chosen_predecessor[step * kNumStates + state];
    state = t.predecessors[state][pick];
  }
  output_bits.resize(num_output_bits);
  return output_bits;
}

}  // namespace weaklink::fec
