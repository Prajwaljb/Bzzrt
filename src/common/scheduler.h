// Test definitions and the frame-interval scheduler that drives presentation timing.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rng.h"

namespace vrr {

enum class TestKind { Stable, Custom, Sweep, Switch, Spikes, Drops, Jitter, Lows };

struct TestDef {
    std::string id;
    std::string name;
    TestKind kind = TestKind::Stable;
    double fpsA = 0;  // stable fps / first switch rate / base rate / sweep min
    double fpsB = 0;  // second switch rate / sweep max
};

struct TestParams {
    double customFps = 60.0;
    double switchPeriodSec = 1.0;  // time spent at each rate in switch tests
    double sweepPeriodSec = 10.0;  // full 45->55->45 cycle
    double spikeRateHz = 0.5;      // average spikes per second
    double spikeMinMs = 25.0;
    double spikeMaxMs = 50.0;
    int spikeFrames = 1;           // consecutive slow frames per spike
    double dropRateHz = 0.3;       // average drops per second
    double dropDurationSec = 0.5;
    double dropMinFps = 40.0;
    double dropMaxFps = 45.0;
    double jitterMinFps = 45.0;
    double jitterMaxFps = 55.0;
    double lowsAvgFps = 60.0;
    double lowsFps = 40.0;
    double lowsPercent = 1.0;
    uint64_t seed = 1;
};

const std::vector<TestDef>& allTests();
int findTest(const std::string& id);
std::string describeTest(const TestDef& def, const TestParams& p);

class Scheduler {
public:
    void start(const TestDef& def, const TestParams& p);
    // Interval (seconds) between the previous and the next present.
    // elapsedSec is the *scheduled* time since test start (sum of previous
    // intervals), so the sequence is deterministic regardless of late frames.
    double nextInterval(double elapsedSec);
    TestParams& params() { return p_; }
    const TestParams& params() const { return p_; }
    const char* phase() const { return phase_; }
    double lastFps() const { return lastFps_; }

private:
    double ret(double fps, const char* phase);
    TestDef def_{};
    TestParams p_{};
    Rng rng_;
    int spikeLeft_ = 0;
    double dropUntil_ = -1.0;
    double dropFps_ = 0.0;
    double lastFps_ = 0.0;
    const char* phase_ = "";
};

}  // namespace vrr
