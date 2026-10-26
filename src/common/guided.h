// Guided flicker check: a fixed sequence of steps, a plain-language verdict on the
// timing the system reported for each step, and the conclusion drawn from those
// verdicts plus the user's own yes/no flicker observations.
#pragma once

#include <string>
#include <vector>

#include "scheduler.h"

namespace vrr {

struct GuidedStep {
    const char* testId;   // scheduler test driving the step
    const char* title;
    const char* explain;  // one sentence shown under the title
    bool changing;        // frame rate changes during the step
    double jitterMin = 0, jitterMax = 0;  // jitter steps override the TestParams range
};

// lowRefresh: the variant for a 60 Hz display mode (VRR range 48-60 Hz), see Platform::setRefreshCeiling.
const std::vector<GuidedStep>& guidedSteps(bool lowRefresh = false);

// AppLate: the app itself missed its own frame times, so the step says nothing about the driver.
enum class Timing { NoData, Even, Uneven, FixedRefresh, AppLate };
constexpr double kAppLateLimitPct = 5.0;
enum class Answer { None, Yes, No, Unsure };

// One display-reported interval and the interval the app asked for, in ms.
struct TimingSample {
    double targetMs;
    double actualMs;
};

struct TimingVerdict {
    Timing timing = Timing::NoData;
    int samples = 0;
    double onTimePct = 0.0;  // share of intervals within tolerance of the requested one
    double commonA = 0.0;    // the two most frequent intervals (1 ms buckets), 0 if none
    double commonB = 0.0;
};

// refreshHz: nominal output refresh rate, used to recognize fixed-refresh quantization (0 = unknown).
TimingVerdict analyzeTiming(const std::vector<TimingSample>& s, double refreshHz);

struct StepResult {
    TimingVerdict verdict;
    Answer answer = Answer::None;
    double targetFps = 0.0;  // steady steps only, 0 for changing steps
    int osdHz = 0;           // refresh rate the user read off the monitor's OSD, 0 = not entered
    double latePct = 0.0;    // frames the app presented more than 0.2 ms after their scheduled time
    std::string csvPath;
};

// What the monitor's own refresh readout (OSD) means at a steady frame rate. With VRR the panel
// refreshes at the frame rate or a whole multiple of it (the driver may show each frame k times).
enum class OsdFit { None, Multiple, Ambiguous, FixedRefresh, AtMinimum, Mismatch };
struct OsdReading {
    OsdFit fit = OsdFit::None;
    int multiple = 0;  // k for Multiple / Ambiguous
};
// vrrMinHz: below it, a reading at the minimum means the monitor is holding frames at its lowest rate.
OsdReading interpretOsd(double fps, int osdHz, double refreshHz, double vrrMinHz = 0);
// e.g. "90 or 180 Hz": the readings that are consistent with VRR at this frame rate.
std::string osdExpected(double fps, double vrrMinHz, double vrrMaxHz);
std::string osdSentence(double fps, int osdHz, double refreshHz, double vrrMinHz = 0);

const char* timingLabel(Timing t);
const char* answerLabel(Answer a);
// Short explanation of a verdict, e.g. "frames alternated 26 / 18 ms instead of an even 22.2 ms".
std::string timingSentence(const GuidedStep& step, const StepResult& r);
// Paragraphs of the final conclusion.
std::vector<std::string> guidedConclusion(const std::vector<StepResult>& results, double vrrMinHz, double refreshHz,
                                          bool lowRefresh = false);

}  // namespace vrr
