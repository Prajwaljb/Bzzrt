#include "csv_log.h"

#include <algorithm>

namespace vrr {

static std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

CsvLogger::~CsvLogger() { close({}); }

bool CsvLogger::open(const std::string& path, const std::vector<std::string>& headerComments, int64_t startNs) {
    close({});
    f_ = std::fopen(path.c_str(), "w");
    if (!f_) return false;
    std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
    path_ = path;
    startNs_ = startNs;
    states_.clear();
    pending_.clear();
    lastDisplayNs_ = 0;
    lastDisplayFrame_ = ~0ull;
    for (const auto& c : headerComments) std::fprintf(f_, "# %s\n", c.c_str());
    std::fprintf(f_,
                 "frame,time_s,host_monotonic_ns,target_frametime_ms,actual_frametime_ms,present_call_ms,"
                 "present_status,late,below_vrr_floor,target_fps,test_mode,window_mode,sync_mode,present_mode,"
                 "color_format,display_mode,display_time_s,display_interval_ms,timing_source\n");
    return true;
}

uint32_t CsvLogger::setState(const LogState& s) {
    if (!states_.empty() && states_.back() == s) return static_cast<uint32_t>(states_.size() - 1);
    states_.push_back(s);
    return static_cast<uint32_t>(states_.size() - 1);
}

void CsvLogger::push(const FrameRecord& r) {
    if (!f_) return;
    pending_.push_back(r);
    flush(256);
}

void CsvLogger::setDisplayTime(uint64_t frame, int64_t ns) {
    if (pending_.empty()) return;
    const uint64_t first = pending_.front().frame;
    if (frame < first) return;
    const uint64_t idx = frame - first;
    if (idx < pending_.size() && pending_[idx].frame == frame) {
        pending_[idx].displayNs = ns;
        return;
    }
    // Frame numbers have gaps when frames were skipped (minimized window, lost surface).
    const auto it = std::lower_bound(pending_.begin(), pending_.end(), frame,
                                     [](const FrameRecord& r, uint64_t f) { return r.frame < f; });
    if (it != pending_.end() && it->frame == frame) it->displayNs = ns;
}

void CsvLogger::flush(size_t keep) {
    while (pending_.size() > keep) {
        const FrameRecord& r = pending_.front();
        const LogState& s = states_[r.state];
        char disp[64] = "";
        char dint[64] = "";
        if (r.displayNs != 0) {
            std::snprintf(disp, sizeof disp, "%.6f", static_cast<double>(r.displayNs - startNs_) / 1e9);
            if (lastDisplayFrame_ != ~0ull && r.frame == lastDisplayFrame_ + 1)
                std::snprintf(dint, sizeof dint, "%.4f", static_cast<double>(r.displayNs - lastDisplayNs_) / 1e6);
            lastDisplayNs_ = r.displayNs;
            lastDisplayFrame_ = r.frame;
        }
        std::fprintf(f_, "%llu,%.6f,%lld,%.4f,%.4f,%.4f,%s,%d,%d,%.3f,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                     static_cast<unsigned long long>(r.frame), r.timeSec, static_cast<long long>(r.hostNs),
                     r.targetMs, r.actualMs, r.presentCallMs, r.status, r.late ? 1 : 0, r.belowFloor ? 1 : 0,
                     r.targetFps, quoted(s.testMode).c_str(), quoted(s.windowMode).c_str(),
                     quoted(s.syncMode).c_str(), quoted(s.presentMode).c_str(), quoted(s.colorFormat).c_str(),
                     quoted(s.displayMode).c_str(), disp, dint, quoted(s.timingSource).c_str());
        pending_.pop_front();
    }
}

void CsvLogger::close(const std::vector<std::string>& footerComments) {
    if (!f_) return;
    flush(0);
    for (const auto& c : footerComments) std::fprintf(f_, "# %s\n", c.c_str());
    std::fclose(f_);
    f_ = nullptr;
}

}  // namespace vrr
