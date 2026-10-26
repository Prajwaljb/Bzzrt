// Deterministic RNG (SplitMix64). std:: distributions are implementation
// defined and would produce different sequences on MSVC and libstdc++; this
// produces bit-identical test sequences on Windows and Linux for a given seed.
#pragma once

#include <cstdint>

namespace vrr {

class Rng {
public:
    explicit Rng(uint64_t seed = 1) : state_(seed) {}
    void seed(uint64_t s) { state_ = s; }

    uint64_t next() {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Uniform in [0, 1).
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double a, double b) { return a + (b - a) * uniform(); }

private:
    uint64_t state_;
};

}  // namespace vrr
