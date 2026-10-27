#include "scheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vrr {

const std::vector<TestDef>& allTests() {
    static const std::vector<TestDef> tests = {
        {"stable200", "Stable 200 FPS", TestKind::Stable, 200, 0},
        {"stable144", "Stable 144 FPS", TestKind::Stable, 144, 0},
        {"stable120", "Stable 120 FPS", TestKind::Stable, 120, 0},
        {"stable100", "Stable 100 FPS", TestKind::Stable, 100, 0},
        {"stable90", "Stable 90 FPS", TestKind::Stable, 90, 0},
        {"stable60", "Stable 60 FPS", TestKind::Stable, 60, 0},
        {"stable55", "Stable 55 FPS", TestKind::Stable, 55, 0},
        {"stable50", "Stable 50 FPS", TestKind::Stable, 50, 0},
        {"stable48", "Stable 48 FPS", TestKind::Stable, 48, 0},
        {"stable47", "Stable 47 FPS", TestKind::Stable, 47, 0},
        {"stable45", "Stable 45 FPS", TestKind::Stable, 45, 0},
        {"stable40", "Stable 40 FPS", TestKind::Stable, 40, 0},
        {"stable30", "Stable 30 FPS", TestKind::Stable, 30, 0},
        {"custom", "Stable custom FPS (+/-)", TestKind::Custom, 0, 0},
        {"lfc50", "LFC: stable 50 FPS (just above a 48 Hz floor)", TestKind::Stable, 50, 0},
        {"lfc47", "LFC: stable 47 FPS (just below a 48 Hz floor)", TestKind::Stable, 47, 0},
        {"sweep", "LFC: smooth 45-55 FPS sweep", TestKind::Sweep, 45, 55},
        {"switch47-50", "LFC: repeated 50 <-> 47 FPS", TestKind::Switch, 50, 47},
        {"switch60-45", "LFC: repeated 60 -> 45 -> 60 FPS", TestKind::Switch, 60, 45},
        {"switch100-45", "LFC: repeated 100 -> 45 -> 100 FPS", TestKind::Switch, 100, 45},
        {"switch200-45", "LFC: repeated 200 -> 45 -> 200 FPS", TestKind::Switch, 200, 45},
        {"spikes60", "Jitter: 60 FPS + 25-50 ms spikes", TestKind::Spikes, 60, 0},
        {"drops55", "Jitter: 55 FPS + drops to 40-45 FPS", TestKind::Drops, 55, 0},
        {"jitter45-55", "Jitter: random 45-55 FPS per frame", TestKind::Jitter, 45, 55},
        {"lows", "Jitter: stable average + configurable 1% lows", TestKind::Lows, 0, 0},
    };
    return tests;
}

int findTest(const std::string& id) {
    const auto& t = allTests();
    for (size_t i = 0; i < t.size(); ++i)
        if (id == t[i].id) return static_cast<int>(i);
    return -1;
}

std::string describeTest(const TestDef& d, const TestParams& p) {
    char buf[256];
    switch (d.kind) {
        case TestKind::Custom:
            std::snprintf(buf, sizeof buf, "Stable custom %.2f FPS (+/-)", p.customFps);
            return buf;
        case TestKind::Sweep:
            std::snprintf(buf, sizeof buf, "%s (period %.1f s)", d.name.c_str(), p.sweepPeriodSec);
            return buf;
        case TestKind::Switch:
            std::snprintf(buf, sizeof buf, "%s (%.2f s each)", d.name.c_str(), p.switchPeriodSec);
            return buf;
        case TestKind::Spikes:
            std::snprintf(buf, sizeof buf, "Jitter: %.0f FPS + %.0f-%.0f ms spikes x%d @ %.2f/s", d.fpsA,
                          p.spikeMinMs, p.spikeMaxMs, p.spikeFrames, p.spikeRateHz);
            return buf;
        case TestKind::Drops:
            std::snprintf(buf, sizeof buf, "Jitter: %.0f FPS + drops to %.0f-%.0f FPS (%.2f s @ %.2f/s)", d.fpsA,
                          p.dropMinFps, p.dropMaxFps, p.dropDurationSec, p.dropRateHz);
            return buf;
        case TestKind::Jitter:
            std::snprintf(buf, sizeof buf, "Jitter: random %.0f-%.0f FPS per frame", p.jitterMinFps,
                          p.jitterMaxFps);
            return buf;
        case TestKind::Lows:
            std::snprintf(buf, sizeof buf, "Jitter: %.1f FPS avg, %.2f%% of frames at %.1f FPS",
                          p.lowsAvgFps, p.lowsPercent, p.lowsFps);
            return buf;
        default:
            return d.name;
    }
}

void Scheduler::start(const TestDef& def, const TestParams& p) {
    def_ = def;
    p_ = p;
    rng_.seed(p.seed);
    spikeLeft_ = 0;
    dropUntil_ = -1.0;
    dropFps_ = 0.0;
}

double Scheduler::ret(double fps, const char* phase) {
    fps = std::max(1.0, fps);
    lastFps_ = fps;
    phase_ = phase;
    return 1.0 / fps;
}

double Scheduler::nextInterval(double t) {
    switch (def_.kind) {
        case TestKind::Stable:
            return ret(def_.fpsA, "steady");
        case TestKind::Custom:
            return ret(p_.customFps, "steady");
        case TestKind::Sweep: {
            const double x = t / std::max(0.1, p_.sweepPeriodSec);
            const double frac = x - std::floor(x);
            const double tri = 1.0 - std::fabs(2.0 * frac - 1.0);  // 0 -> 1 -> 0
            return ret(def_.fpsA + (def_.fpsB - def_.fpsA) * tri, frac < 0.5 ? "rising" : "falling");
        }
        case TestKind::Switch: {
            const long long phase = static_cast<long long>(std::floor(t / std::max(0.01, p_.switchPeriodSec)));
            return (phase % 2 == 0) ? ret(def_.fpsA, "high") : ret(def_.fpsB, "low");
        }
        case TestKind::Spikes: {
            const double base = def_.fpsA;
            if (spikeLeft_ > 0) {
                --spikeLeft_;
                return ret(1000.0 / rng_.uniform(p_.spikeMinMs, p_.spikeMaxMs), "SPIKE");
            }
            if (rng_.uniform() < p_.spikeRateHz / base) {
                spikeLeft_ = std::max(1, p_.spikeFrames) - 1;
                return ret(1000.0 / rng_.uniform(p_.spikeMinMs, p_.spikeMaxMs), "SPIKE");
            }
            return ret(base, "base");
        }
        case TestKind::Drops: {
            const double base = def_.fpsA;
            if (t < dropUntil_) return ret(dropFps_, "DROP");
            if (rng_.uniform() < p_.dropRateHz / base) {
                dropUntil_ = t + p_.dropDurationSec;
                dropFps_ = rng_.uniform(p_.dropMinFps, p_.dropMaxFps);
                return ret(dropFps_, "DROP");
            }
            return ret(base, "base");
        }
        case TestKind::Jitter:
            return ret(rng_.uniform(p_.jitterMinFps, p_.jitterMaxFps), "jitter");
        case TestKind::Lows: {
            const double pct = std::clamp(p_.lowsPercent, 0.0, 50.0) / 100.0;
            if (rng_.uniform() < pct) return ret(p_.lowsFps, "LOW");
            // Normal frames are shortened so the long-run average stays at lowsAvgFps.
            double normal = (1.0 / p_.lowsAvgFps - pct / p_.lowsFps) / (1.0 - pct);
            if (normal <= 0.0) normal = 1.0 / p_.lowsAvgFps;
            return ret(1.0 / normal, "normal");
        }
    }
    return ret(60.0, "");
}

}  // namespace vrr
