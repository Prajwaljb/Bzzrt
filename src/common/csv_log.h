// Per-frame CSV logger. Rows are held back briefly so driver-reported
// presentation timestamps (which arrive a few frames late) can be attached.
#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace vrr {

struct LogState {
    std::string testMode;
    std::string windowMode;
    std::string syncMode;
    std::string presentMode;
    std::string colorFormat;
    std::string displayMode;
    std::string timingSource;
    bool operator==(const LogState& o) const {
        return testMode == o.testMode && windowMode == o.windowMode && syncMode == o.syncMode &&
               presentMode == o.presentMode && colorFormat == o.colorFormat && displayMode == o.displayMode &&
               timingSource == o.timingSource;
    }
};

struct FrameRecord {
    uint64_t frame = 0;
    int64_t hostNs = 0;          // nowNs() when Present / flip was called
    double timeSec = 0;          // since test start
    double targetMs = 0;
    double actualMs = 0;
    double presentCallMs = 0;
    double targetFps = 0;
    const char* status = "";
    bool late = false;
    bool belowFloor = false;
    uint32_t state = 0;
    int64_t displayNs = 0;       // 0 = not reported
};

class CsvLogger {
public:
    ~CsvLogger();
    bool open(const std::string& path, const std::vector<std::string>& headerComments, int64_t startNs);
    bool isOpen() const { return f_ != nullptr; }
    const std::string& path() const { return path_; }
    uint32_t setState(const LogState& s);
    void push(const FrameRecord& r);
    void setDisplayTime(uint64_t frame, int64_t ns);
    void close(const std::vector<std::string>& footerComments);

private:
    void flush(size_t keep);
    FILE* f_ = nullptr;
    std::string path_;
    std::deque<FrameRecord> pending_;
    std::vector<LogState> states_;
    int64_t startNs_ = 0;
    int64_t lastDisplayNs_ = 0;
    uint64_t lastDisplayFrame_ = ~0ull;
};

}  // namespace vrr
