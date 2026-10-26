#include "stats.h"

#include <algorithm>
#include <cmath>

namespace vrr {

FrameStats::FrameStats() : fine_(kFineBins, 0) {}

void FrameStats::reset(double vrrMinHz, double vrrMaxHz) {
    std::fill(fine_.begin(), fine_.end(), 0u);
    hist_.fill(0);
    recent_.clear();
    count_ = 0;
    sumMs_ = maxMs_ = lastMs_ = 0;
    maxFrame_ = 0;
    late_ = belowFloor_ = aboveCeiling_ = crossings_ = 0;
    lastSide_ = 0;
    floorMs_ = 1000.0 / vrrMinHz;
    ceilingMs_ = 1000.0 / vrrMaxHz;
    presentSumMs_ = presentMaxMs_ = presentLastMs_ = 0;
    dispCount_ = 0;
    dispSumMs_ = dispMaxMs_ = dispMinMs_ = dispLastMs_ = 0;
}

void FrameStats::addFrame(int64_t presentNs, uint64_t frameIndex, double ft, double presentCallMs, bool late) {
    recent_.push_back(presentNs);
    while (!recent_.empty() && presentNs - recent_.front() > 1000000000LL) recent_.pop_front();

    ++count_;
    sumMs_ += ft;
    lastMs_ = ft;
    if (ft > maxMs_) {
        maxMs_ = ft;
        maxFrame_ = frameIndex;
    }
    const int fb = std::clamp(static_cast<int>(ft / kFineBinMs), 0, kFineBins - 1);
    ++fine_[fb];
    const int hb = std::clamp(static_cast<int>(ft / kHistBinMs), 0, kHistBins);
    ++hist_[hb];

    if (late) ++late_;
    // A small tolerance avoids counting timer noise at exactly 48 / 200 FPS.
    const int side = ft > floorMs_ * 1.003 ? -1 : 1;
    if (side < 0) ++belowFloor_;
    if (ft < ceilingMs_ * 0.97) ++aboveCeiling_;
    if (lastSide_ != 0 && side != lastSide_) ++crossings_;
    lastSide_ = side;

    presentSumMs_ += presentCallMs;
    presentLastMs_ = presentCallMs;
    presentMaxMs_ = std::max(presentMaxMs_, presentCallMs);
}

void FrameStats::addDisplayInterval(double ms) {
    if (dispCount_ == 0) dispMinMs_ = ms;
    ++dispCount_;
    dispSumMs_ += ms;
    dispLastMs_ = ms;
    dispMaxMs_ = std::max(dispMaxMs_, ms);
    dispMinMs_ = std::min(dispMinMs_, ms);
}

double FrameStats::percentileMs(double pct) const {
    if (count_ == 0) return 0.0;
    const double want = std::ceil(static_cast<double>(count_) * pct / 100.0);
    uint64_t acc = 0;
    for (int i = 0; i < kFineBins; ++i) {
        acc += fine_[i];
        if (static_cast<double>(acc) >= want) return std::min((i + 1) * kFineBinMs, maxMs_);
    }
    return maxMs_;
}

double FrameStats::rollingFps(int64_t nowNs) const {
    if (recent_.size() < 2) return 0.0;
    if (nowNs - recent_.back() > 1000000000LL) return 0.0;
    const double span = static_cast<double>(recent_.back() - recent_.front()) / 1e9;
    return span > 0 ? static_cast<double>(recent_.size() - 1) / span : 0.0;
}

}  // namespace vrr
