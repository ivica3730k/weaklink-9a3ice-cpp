#pragma once

#include <stdexcept>
#include <string>

namespace weaklink {

/// Base for anything the modem raises. Catch this to catch them all.
class WeaklinkError : public std::runtime_error {
 public:
  explicit WeaklinkError(const std::string& what) : std::runtime_error(what) {}
};

/// Invalid configuration -- baud not supported, tone count not a power of 2,
/// block_repeats < 1, PTT endpoint malformed, etc.
class ConfigError : public WeaklinkError {
 public:
  explicit ConfigError(const std::string& what) : WeaklinkError(what) {}
};

/// MFSK tone stack won't fit under sample_rate/2, or spacing is below the
/// non-coherent orthogonality floor.
class NyquistError : public ConfigError {
 public:
  explicit NyquistError(const std::string& what) : ConfigError(what) {}
};

/// Encoder-side failure -- e.g. stream exceeded the 2^16 slots per session.
class EncodeError : public WeaklinkError {
 public:
  explicit EncodeError(const std::string& what) : WeaklinkError(what) {}
};

/// rigctld connect / response failure.
class PTTError : public WeaklinkError {
 public:
  explicit PTTError(const std::string& what) : WeaklinkError(what) {}
};

}  // namespace weaklink
