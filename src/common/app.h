// Platform independent main loop: frame pacing, test control, statistics, overlay and logging.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "backend.h"
#include "config.h"
#include "csv_log.h"
#include "guided.h"
#include "overlay.h"
#include "scheduler.h"
#include "stats.h"

namespace vrr {

class App {
public:
    App(const Config& cfg, Platform& platform, Backend& backend);
    int run();

private:
    enum class Mode { Intro, Step, Results, Advanced };

    void handleKey(const KeyEvent& k);
    void handleGuidedKey(const KeyEvent& k);
    void selectTest(int index);
    void startTest(int64_t now, const TestParams& p);
    void stopTest(const char* reason);
    void refreshInfo();
    void refreshLogState();
    void buildOverlay(int64_t now);
    void buildGuidedOverlay(int64_t now);
    void startStep(int index);
    void finishStep();
    void leaveGuided(Mode to);
    void writeReport();
    void flash(const std::string& msg);
    std::string testLabel() const;
    std::vector<std::string> summaryLines() const;
    std::vector<std::string> headerLines() const;

    Config cfg_;
    Mode mode_ = Mode::Intro;
    Platform& platform_;
    Backend& backend_;

    Scheduler sched_;
    TestParams params_;
    int testIdx_ = 0;
    bool running_ = false;
    bool haveResults_ = false;
    int64_t testStartNs_ = 0;
    uint64_t testStartFrame_ = 0;
    double schedElapsed_ = 0.0;
    double duration_ = 60.0;
    std::string lastStopReason_;

    FrameStats stats_;   // current / last test run
    FrameStats live_;    // always running, for live FPS
    CsvLogger log_;
    uint32_t logState_ = 0;
    bool logging_ = true;
    std::string lastLogPath_;

    Overlay overlay_;
    int64_t lastOverlayNs_ = 0;
    int64_t appStartNs_ = 0;
    uint64_t frame_ = 0;
    int pattern_ = 0;
    bool overlayVisible_ = true;
    bool counter_ = false;
    bool quit_ = false;

    BackendInfo binfo_;
    DisplayInfo dinfo_;
    double curTargetMs_ = 0.0;
    const char* lastStatus_ = "";
    const char* phase_ = "";
    uint64_t lastDispFrame_ = ~0ull;
    int64_t lastDispNs_ = 0;

    // Guided check
    static constexpr int kTargetRing = 1024;
    std::array<double, kTargetRing> targetRing_{};  // requested interval (ms) per frame index
    int stepIdx_ = 0;
    int64_t stepStartNs_ = 0;
    int64_t answerNs_ = 0;
    Answer answer_ = Answer::None;
    std::string osdInput_;  // digits typed for the monitor's OSD refresh reading
    std::vector<TimingSample> stepSamples_;
    std::vector<StepResult> results_;
    std::string reportPath_;
    std::string logPrefix_;
    int overlayX_ = 16, overlayY_ = 16;

    std::string message_;
    int64_t messageUntil_ = 0;
};

}  // namespace vrr
