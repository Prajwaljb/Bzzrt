// Platform independent main loop: frame pacing, test control, statistics, overlay and logging.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "backend.h"
#include "config.h"
#include "csv_log.h"
#include "guided.h"
#include "overlay.h"
#include "range_store.h"
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
    void requestStart();

    // VRR range: command line > entered by the user (remembered per monitor) > EDID / OS > unknown (asked).
    void resolveRange(bool force = false);
    bool rangeKnown() const { return vrrMin_ > 0 && vrrMax_ > vrrMin_; }
    bool detectedKnown() const { return detMin_ > 0 && detMax_ > detMin_; }
    bool usingManual() const;
    void applyRange();
    void toggleRangeSource();
    double rangeMin() const;      // fallbacks keep the advanced bench usable while the range is unknown
    double rangeMax() const;      // effective maximum: limited by the display mode and the low-refresh test
    double lfcThreshold() const;  // frame rate below which frames should be shown several times
    bool modeLimited() const;     // the display mode's refresh rate is below the monitor's VRR maximum
    void rebuildSteps();
    CheckContext checkContext() const;
    void beginRangeEdit(bool thenStart);
    void handleRangeKey(const KeyEvent& k);
    void addRangeLines(const std::function<void(const std::string&, uint32_t)>& add);
    void addEditorLines(const std::function<void(const std::string&, uint32_t)>& add);
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
    TestDef curDef_;  // test being run: the advanced bench's selection or the current guided step
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

    // VRR range
    double vrrMin_ = 0, vrrMax_ = 0, lfcFps_ = 0;  // the range in use
    std::string rangeSource_;
    double detMin_ = 0, detMax_ = 0;  // detected from the EDID / OS, 0 = nothing detected
    std::string detSource_;
    SavedRange manual_;               // the user's entry for this monitor
    bool haveManual_ = false;
    std::string rangeKey_;
    bool rangeResolved_ = false;
    bool rangeAnyFromCli_ = false;
    int editField_ = 0;  // range editor: 0 closed, 1 minimum, 2 maximum, 3 LFC threshold
    std::string editMin_, editMax_, editLfc_, editError_, rangeNote_;
    bool startAfterEdit_ = false;

    // Guided check
    std::vector<GuidedStep> steps_;
    static constexpr int kTargetRing = 1024;
    std::array<double, kTargetRing> targetRing_{};  // requested interval (ms) per frame index
    int stepIdx_ = 0;
    int64_t stepStartNs_ = 0;
    int64_t answerNs_ = 0;
    Answer answer_ = Answer::None;
    std::string osdInput_;    // digits typed for the monitor's OSD refresh reading
    double lowHz_ = 0.0;      // low-refresh test: display-mode refresh rate in use, 0 = off
    std::string lowStatus_;   // why the low-refresh test could not start
    std::vector<TimingSample> stepSamples_;
    std::vector<StepResult> results_;
    // The finished run's steps and range: the live ones can change afterwards (another monitor, a new range).
    std::vector<GuidedStep> resultSteps_;
    CheckContext resultCtx_;
    std::string reportPath_;
    std::string logPrefix_;
    std::string logId_;  // overrides the test id in CSV file names (guided steps with custom parameters)
    int overlayX_ = 16, overlayY_ = 16;
    StepResult liveStep_;          // live verdict on the step screen, refreshed once per second
    int64_t liveStepNs_ = 0;
    std::string guidedSignature_;  // content of the last guided overlay; unchanged text is not redrawn
    int64_t overlayCostNs_ = 0;    // extra time an overlay change adds to a frame (build + extra render), decays slowly
    int64_t renderNormalNs_ = 0;   // average renderFrame time without an overlay change

    // Frame doubling experiment (--double-frames)
    int repeatMultiple(double intervalSec) const;
    bool doubling_ = false;
    FrameParams lastFp_;           // the frame last presented, shown again for each repeat
    bool haveLastFp_ = false;
    double repeatExpectMs_ = 0.0;  // requested gap between the flips while repeating
    int64_t lastAnyFlipNs_ = 0;    // last flip of a frame or a repeat
    int stepRepeats_ = 0, stepRepeatsOk_ = 0;

    std::string message_;
    int64_t messageUntil_ = 0;
};

}  // namespace vrr
