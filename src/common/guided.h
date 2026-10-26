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

const std::vector<GuidedStep>& guidedSteps();

enum class Timing { NoData, Even, Uneven, FixedRefresh };
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
    std::string csvPath;
};

const char* timingLabel(Timing t);
const char* answerLabel(Answer a);
// Short explanation of a verdict, e.g. "frames alternated 26 / 18 ms instead of an even 22.2 ms".
std::string timingSentence(const GuidedStep& step, const StepResult& r);
// Paragraphs of the final conclusion.
std::vector<std::string> guidedConclusion(const std::vector<StepResult>& results, double vrrMinHz, double refreshHz);

}  // namespace vrr
