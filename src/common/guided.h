// Guided flicker check: a fixed sequence of steps, a plain-language verdict on the
// timing the system reported for each step, and the conclusion drawn from those
// verdicts plus the user's own yes/no flicker observations.
#pragma once

#include <string>
#include <vector>

#include "scheduler.h"

namespace vrr {

struct GuidedStep {
    TestDef test;         // scheduler test driving the step
    std::string logId;    // test id in CSV file names, e.g. "stable45"
    std::string title;
    std::string explain;  // one sentence shown under the title
    bool changing = false;                // frame rate changes during the step
    double jitterMin = 0, jitterMax = 0;  // jitter steps override the TestParams range
};

// Frame doubling (LFC) needs room for a doubled frame inside the range: maximum >= 2 x minimum.
bool lfcPossible(double vrrMinHz, double vrrMaxHz);

// The check's steps, built around the monitor's VRR range: in range, just above the minimum, below it, far below
// it, and frame rates that cross it. ceilingHz > 0 builds the low-refresh variant for a display mode of that
// refresh rate (see Platform::setRefreshCeiling), in which frames below the minimum cannot be shown twice.
std::vector<GuidedStep> guidedSteps(double vrrMinHz, double vrrMaxHz, double ceilingHz = 0);

// The refresh rate to use for the low-refresh variant: one of the available rates that is above the minimum but
// below twice of it, closest to 1.25 x the minimum. 0 if none fits.
double pickRefreshCeiling(double vrrMinHz, const std::vector<double>& availableHz);

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
    // Frame doubling experiment: repeat refreshes the app added, and how many landed at the requested time.
    int multiple = 1;        // times each frame was shown (steady steps)
    int repeats = 0;
    double repeatOkPct = 0.0;
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
struct CheckContext {
    double vrrMinHz = 0, vrrMaxHz = 0;  // range the steps were built for (maximum limited by the display mode)
    double monitorMaxHz = 0;            // the monitor's own VRR maximum
    double refreshHz = 0;               // display mode refresh rate, 0 = unknown
    bool lowRefresh = false;            // low-refresh variant
    bool doubling = false;              // the app showed slow frames several times itself (--double-frames)
};
// Paragraphs of the final conclusion.
std::vector<std::string> guidedConclusion(const std::vector<GuidedStep>& steps, const std::vector<StepResult>& results,
                                          const CheckContext& ctx);

}  // namespace vrr
