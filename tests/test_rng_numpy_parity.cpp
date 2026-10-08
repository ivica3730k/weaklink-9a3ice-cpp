// The generator is wire-format-defining: the interleaver permutation and the
// pilot symbol sequence both come out of it, so a C++ receiver that drew
// different numbers could not decode a Python transmitter's audio.
//
// Every expectation below was produced by NumPy itself (numpy 2.5.1) and is
// pasted verbatim. If one of these fails, interoperability is broken even if
// every higher-level test still passes against C++-generated audio.

#include "catch.hpp"
#include <string>
#include <vector>

#include "weaklink/rng.hpp"

using weaklink::rng::NumpyGenerator;
using weaklink::rng::PythonRandom;

TEST_CASE("raw PCG64 stream matches numpy", "[rng]") {
  SECTION("seed 0") {
    NumpyGenerator generator(0);
    const std::vector<uint64_t> expected = {
        11749869230777074271ULL, 4976686463289251617ULL,  755828109848996024ULL,
        304881062738325533ULL,   15002187965291974971ULL, 16837368535893154894ULL,
        11190454901533422207ULL, 13456836363123071557ULL};
    for (const uint64_t want : expected) {
      REQUIRE(generator.next_uint64() == want);
    }
  }

  SECTION("interleaver base seed") {
    // 0xC0DEC0DE is the interleaver's slot-0 seed, so this stream is the one
    // that actually shapes the wire format.
    NumpyGenerator generator(0xC0DEC0DEULL);
    const std::vector<uint64_t> expected = {
        6455851057825311438ULL,  11610557928479555792ULL, 6551601671093870981ULL,
        2805556660852815974ULL,  2389688844099331918ULL,  12027053946338874403ULL,
        13030789665540792424ULL, 6063508042685741444ULL};
    for (const uint64_t want : expected) {
      REQUIRE(generator.next_uint64() == want);
    }
  }
}

TEST_CASE("random() doubles match numpy", "[rng]") {
  NumpyGenerator generator(0);
  const std::vector<double> expected = {0.6369616873214543,  0.2697867137638703,
                                        0.04097352393619469, 0.016527635528529094,
                                        0.8132702392002724,  0.9127555772777217,
                                        0.6066357757671799,  0.7294965609839984};
  for (const double want : expected) {
    REQUIRE(generator.next_double() == Approx(want).epsilon(0).margin(0));
  }
}

TEST_CASE("bytes() matches numpy", "[rng]") {
  NumpyGenerator generator(80);
  const std::vector<uint8_t> expected = {62, 182, 90, 232, 37,  178, 138, 188,
                                         243, 125, 70, 0,   34, 24,  183, 4};
  REQUIRE(generator.bytes(expected.size()) == expected);
}

TEST_CASE("integers() matches numpy", "[rng]") {
  // Generator.integers routes through Lemire's sampler, which consumes the
  // stream differently from the masked rejection shuffle uses.
  NumpyGenerator generator(0xC0DEULL);
  const std::vector<int> expected = {3, 1, 1, 2, 0, 2, 2, 1, 3, 2, 1, 2,
                                     1, 3, 2, 3, 1, 3, 0, 3, 3, 1, 0, 0};
  for (const int want : expected) {
    REQUIRE(static_cast<int>(generator.bounded(4)) == want);
  }
}

TEST_CASE("permutation() matches numpy", "[rng]") {
  // NumPy's shuffle uses masked rejection sampling, not Lemire. Getting this
  // wrong yields a valid-looking permutation that no Python receiver agrees
  // with, so it is worth pinning explicitly.
  NumpyGenerator generator(0xC0DEC0DEULL);
  const std::vector<uint32_t> expected = {17, 26, 10, 0,  23, 30, 22, 25, 18, 12, 20,
                                          15, 27, 13, 19, 24, 1,  7,  29, 4,  9,  8,
                                          3,  31, 28, 6,  2,  5,  11, 16, 21, 14};
  REQUIRE(generator.permutation(32) == expected);
}

TEST_CASE("standard_normal() matches numpy", "[rng]") {
  // The ziggurat tables have to be NumPy's exact constants; a table rebuilt
  // from the published construction drifts in the 12th significant digit.
  struct Case {
    uint64_t seed;
    std::vector<double> expected;
  };
  const std::vector<Case> cases = {
      {0,
       {0.1257302210933933, -0.1321048632913019, 0.6404226504432821, 0.10490011715303971,
        -0.535669373161111, 0.36159505490948474, 1.3040000451301372, 0.9470809631292422,
        -0.7037352358069926, -1.2654214710460525, -0.6232744625373522, 0.0413259793472436}},
      {1,
       {0.345584192064786, 0.8216181435011584, 0.33043707618338714, -1.303157231604361,
        0.9053558666731177, 0.4463745723640113, -0.5369532353602852, 0.5811181041963531,
        0.36457239618607573, 0.294132496655526, 0.02842224131579679, 0.5467129866124469}},
      {42,
       {0.30471707975443135, -1.0399841062404955, 0.7504511958064572, 0.9405647163912139,
        -1.9510351886538364, -1.302179506862318, 0.12784040316728537, -0.3162425923435822,
        -0.016801157504288795, -0.85304392757358, 0.8793979748628286, 0.7777919354289483}},
  };
  for (const Case& test_case : cases) {
    NumpyGenerator generator(test_case.seed);
    for (const double want : test_case.expected) {
      REQUIRE(generator.standard_normal() == Approx(want).epsilon(0).margin(0));
    }
  }
}

TEST_CASE("PythonRandom matches random.Random", "[rng]") {
  SECTION("random() stream") {
    // random.Random(0).random() repeated.
    PythonRandom generator(0);
    const std::vector<double> expected = {0.8444218515250481, 0.7579544029403025,
                                          0.420571580830845, 0.25891675029296335,
                                          0.5112747213686085};
    for (const double want : expected) {
      REQUIRE(generator.random() == Approx(want).epsilon(0).margin(0));
    }
  }

  SECTION("choices() payload") {
    // The benchmark seeds random.Random(0) and draws from
    // ascii_letters + digits + " ".
    std::vector<uint8_t> alphabet;
    for (char c = 'a'; c <= 'z'; ++c) {
      alphabet.push_back(static_cast<uint8_t>(c));
    }
    for (char c = 'A'; c <= 'Z'; ++c) {
      alphabet.push_back(static_cast<uint8_t>(c));
    }
    for (char c = '0'; c <= '9'; ++c) {
      alphabet.push_back(static_cast<uint8_t>(c));
    }
    alphabet.push_back(static_cast<uint8_t>(' '));

    PythonRandom generator(0);
    const std::vector<uint8_t> drawn = generator.choices(alphabet, 16);
    REQUIRE(std::string(drawn.begin(), drawn.end()) == "1VAqGzXtEK5FrVMp");

    PythonRandom other(7);
    const std::vector<uint8_t> short_draw = other.choices(alphabet, 5);
    REQUIRE(std::string(short_draw.begin(), short_draw.end()) == "ujPeH");
  }

  SECTION("randint() and choice()") {
    // Both go through _randbelow_with_getrandbits, a different consumption
    // pattern from random().
    PythonRandom generator(42);
    const std::vector<int64_t> expected = {4, 1, 9, 8, 8};
    for (const int64_t want : expected) {
      REQUIRE(generator.randint(1, 20) == want);
    }

    std::vector<uint8_t> alphabet;
    for (char c = 'a'; c <= 'z'; ++c) {
      alphabet.push_back(static_cast<uint8_t>(c));
    }
    for (char c = 'A'; c <= 'Z'; ++c) {
      alphabet.push_back(static_cast<uint8_t>(c));
    }
    for (char c = '0'; c <= '9'; ++c) {
      alphabet.push_back(static_cast<uint8_t>(c));
    }
    alphabet.push_back(static_cast<uint8_t>(' '));

    PythonRandom picker(342);
    std::string picked;
    for (int i = 0; i < 5; ++i) {
      picked.push_back(static_cast<char>(picker.choice(alphabet)));
    }
    REQUIRE(picked == "hnLee");
  }
}
