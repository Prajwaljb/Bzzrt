// Frame-time statistics for one test run.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace vrr {

class FrameStats {
public:
    static constexpr int kHistBins = 100;          // 0.5 ms bins covering 0..50 ms
    static constexpr double kHistBinMs = 0.5;

    FrameStats();
    void reset(double vrrMinHz, double vrrMaxHz);
    void addFrame(int64_t presentNs, uint64_t frameIndex, double frametimeMs, double presentCallMs, bool late);
    void addDisplayInterval(double ms);

    uint64_t frames() const { return count_; }
    double lastMs() const { return lastMs_; }
    double avgMs() const { return count_ ? sumMs_ / static_cast<double>(count_) : 0.0; }
    double maxMs() const { return maxMs_; }
    uint64_t maxFrame() const { return maxFrame_; }
    // Frame time at the given percentile (e.g. 99.0 -> "1% low" frame time).
    double percentileMs(double pct) const;
    double rollingFps(int64_t nowNs) const;

    uint64_t lateFrames() const { return late_; }
    uint64_t belowFloor() const { return belowFloor_; }
    uint64_t aboveCeiling() const { return aboveCeiling_; }
    uint64_t floorCrossings() const { return crossings_; }
    double floorMs() const { return floorMs_; }
    double ceilingMs() const { return ceilingMs_; }

    double presentCallAvgMs() const { return count_ ? presentSumMs_ / static_cast<double>(count_) : 0.0; }
    double presentCallMaxMs() const { return presentMaxMs_; }
    double presentCallLastMs() const { return presentLastMs_; }

    uint64_t displayCount() const { return dispCount_; }
    double displayAvgMs() const { return dispCount_ ? dispSumMs_ / static_cast<double>(dispCount_) : 0.0; }
    double displayMaxMs() const { return dispMaxMs_; }
    double displayMinMs() const { return dispMinMs_; }
    double displayLastMs() const { return dispLastMs_; }

    const std::array<uint64_t, kHistBins + 1>& histogram() const { return hist_; }

private:
    static constexpr double kFineBinMs = 0.01;
    static constexpr int kFineBins = 100000;  // up to 1000 ms
    std::vector<uint32_t> fine_;
    std::array<uint64_t, kHistBins + 1> hist_{};
    std::deque<int64_t> recent_;
    uint64_t count_ = 0;
    double sumMs_ = 0, maxMs_ = 0, lastMs_ = 0;
    uint64_t maxFrame_ = 0;
    uint64_t late_ = 0, belowFloor_ = 0, aboveCeiling_ = 0, crossings_ = 0;
    int lastSide_ = 0;  // -1 below floor (LFC territory), +1 inside range
    double floorMs_ = 1000.0 / 48.0, ceilingMs_ = 1000.0 / 200.0;
    double presentSumMs_ = 0, presentMaxMs_ = 0, presentLastMs_ = 0;
    uint64_t dispCount_ = 0;
    double dispSumMs_ = 0, dispMaxMs_ = 0, dispMinMs_ = 0, dispLastMs_ = 0;
};

}  // namespace vrr
