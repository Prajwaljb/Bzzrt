#include "guided.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace vrr {

namespace {

std::string fmt(const char* f, double a = 0, double b = 0, double c = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

std::string stepList(const std::vector<int>& idx) {
    std::string s;
    for (size_t i = 0; i < idx.size(); ++i) {
        if (i) s += (i + 1 == idx.size()) ? " and " : ", ";
        s += std::to_string(idx[i] + 1);
    }
    return s;
}

}  // namespace

const std::vector<GuidedStep>& guidedSteps() {
    static const std::vector<GuidedStep> steps = {
        {"stable90", "90 FPS, steady", "Inside the VRR range. A working setup shows a perfectly stable image.", false},
        {"stable60", "60 FPS, steady", "Inside the VRR range. Should look perfectly stable.", false},
        {"stable50", "50 FPS, steady", "Just above the bottom of the VRR range.", false},
        {"jitter45-55", "Changing 55-80 FPS", "The frame rate changes every frame but stays inside the VRR range.", true,
         55, 80},
        {"stable45", "45 FPS, steady",
         "Below the VRR range: every frame must be shown twice (LFC, low framerate compensation).", false},
        {"stable30", "30 FPS, steady", "Far below the VRR range: every frame is shown twice (LFC).", false},
        {"switch60-45", "Switching 60 <-> 45 FPS", "Crosses the bottom of the VRR range every second. This is the most "
                                                   "common trigger of VRR flicker.", true},
        {"jitter45-55", "Changing 45-55 FPS", "Random frame rate around the bottom of the VRR range.", true, 45, 55},
    };
    return steps;
}

TimingVerdict analyzeTiming(const std::vector<TimingSample>& s, double refreshHz) {
    TimingVerdict v;
    v.samples = static_cast<int>(s.size());
    if (s.size() < 20) return v;

    size_t onTime = 0, quantized = 0;
    std::map<long, size_t> buckets;
    const double period = refreshHz > 0 ? 1000.0 / refreshHz : 0.0;
    for (const auto& x : s) {
        // Present-wait completion times are polled (+/-0.2 ms) and compositors add ~1 ms of jitter.
        const double tol = std::max(1.5, 0.08 * x.targetMs);
        if (std::fabs(x.actualMs - x.targetMs) <= tol) ++onTime;
        if (period > 0 && std::fabs(x.actualMs - std::round(x.actualMs / period) * period) <= 0.6) ++quantized;
        ++buckets[std::lround(x.actualMs)];
    }
    v.onTimePct = 100.0 * static_cast<double>(onTime) / static_cast<double>(s.size());

    std::vector<std::pair<size_t, long>> top;
    for (const auto& b : buckets) top.push_back({b.second, b.first});
    std::sort(top.rbegin(), top.rend());
    v.commonA = static_cast<double>(top[0].second);
    if (top.size() > 1 && top[1].first * 100 >= s.size() * 15) v.commonB = static_cast<double>(top[1].second);

    if (v.onTimePct >= 90.0) v.timing = Timing::Even;
    else if (period > 0 && quantized * 100 >= s.size() * 80) v.timing = Timing::FixedRefresh;
    else v.timing = Timing::Uneven;
    return v;
}

const char* timingLabel(Timing t) {
    switch (t) {
        case Timing::Even: return "EVEN";
        case Timing::Uneven: return "UNEVEN";
        case Timing::FixedRefresh: return "FIXED REFRESH";
        default: return "NO DATA";
    }
}

const char* answerLabel(Answer a) {
    switch (a) {
        case Answer::Yes: return "FLICKER";
        case Answer::No: return "no flicker";
        case Answer::Unsure: return "not sure";
        default: return "-";
    }
}

std::string timingSentence(const GuidedStep& step, const StepResult& r) {
    const auto& v = r.verdict;
    const double targetMs = r.targetFps > 0 ? 1000.0 / r.targetFps : 0.0;
    switch (v.timing) {
        case Timing::NoData:
            return "The system did not report when frames reached the screen.";
        case Timing::Even:
            if (!step.changing) return fmt("Frames reached the screen at an even %.1f ms, as requested.", targetMs);
            return fmt("Frames reached the screen when requested (%.0f%% on time).", v.onTimePct);
        case Timing::FixedRefresh:
            return "Frames were snapped to the fixed refresh rate: VRR was NOT active.";
        case Timing::Uneven:
            if (!step.changing && v.commonB > 0)
                return fmt("Frames alternated between ~%.0f and ~%.0f ms instead of an even %.1f ms.", v.commonA,
                           v.commonB, targetMs);
            return fmt("Frames reached the screen unevenly (only %.0f%% on time).", v.onTimePct);
    }
    return "";
}

std::vector<std::string> guidedConclusion(const std::vector<StepResult>& results, double vrrMinHz, double refreshHz) {
    const auto& steps = guidedSteps();
    std::vector<std::string> out;
    std::vector<int> evenSteady, evenChanging, uneven, unknown, noVrr, unevenQuiet;
    int answered = 0, vrrProof = 0, noData = 0;
    const double period = refreshHz > 0 ? 1000.0 / refreshHz : 0.0;

    for (size_t i = 0; i < results.size() && i < steps.size(); ++i) {
        const auto& r = results[i];
        const Timing t = r.verdict.timing;
        if (t == Timing::NoData) ++noData;
        if (t == Timing::Uneven && r.answer != Answer::Yes) unevenQuiet.push_back(static_cast<int>(i));
        // A steady in-range rate shown at even intervals that are not a multiple of the fixed refresh
        // period can only happen with variable refresh.
        if (!steps[i].changing && r.targetFps >= vrrMinHz) {
            const double ms = 1000.0 / r.targetFps;
            const bool multiple = period > 0 && std::fabs(ms - std::round(ms / period) * period) < 0.7;
            if (t == Timing::Even && !multiple) ++vrrProof;
            if (t == Timing::FixedRefresh || (t == Timing::Uneven && !multiple)) noVrr.push_back(static_cast<int>(i));
        }
        if (r.answer != Answer::None && r.answer != Answer::Unsure) ++answered;
        if (r.answer != Answer::Yes) continue;
        if (t == Timing::Even) (steps[i].changing ? evenChanging : evenSteady).push_back(static_cast<int>(i));
        else if (t == Timing::NoData) unknown.push_back(static_cast<int>(i));
        else uneven.push_back(static_cast<int>(i));
    }

    // 1. Was VRR actually in use?
    if (noData == static_cast<int>(results.size())) {
        out.push_back("This system cannot report when frames reach the screen (common with X11 / XWayland), so only "
                      "your own observations count. Try the Wayland session on Linux, or Windows.");
    } else if (vrrProof > 0) {
        out.push_back("VRR WAS ACTIVE: steady frame rates inside the range reached the screen at even intervals that "
                      "a fixed refresh rate cannot produce.");
    } else if (!noVrr.empty()) {
        out.push_back("VRR WAS NOT ACTIVE (steps " + stepList(noVrr) + " were locked to the fixed refresh rate), so "
                      "this run did not test VRR. Turn on Adaptive-Sync / FreeSync in the monitor menu and in the "
                      "system (KDE: System Settings > Display > Adaptive sync = Always; Windows: Settings > Display > "
                      "Graphics > Variable refresh rate), keep this app fullscreen, then run the check again."
#ifdef _WIN32
                      " If it stays off, press F on the start screen until the window mode says Exclusive "
                      "fullscreen and run it once more."
#endif
                      );
    }

    // 2. Where did flicker appear, and was the computer's timing clean there?
    if (answered == 0 && evenSteady.empty() && evenChanging.empty() && uneven.empty() && unknown.empty()) {
        out.push_back("No answers were recorded, so there is nothing to conclude about flicker.");
    } else if (evenSteady.empty() && evenChanging.empty() && uneven.empty() && unknown.empty()) {
        out.push_back("You saw no flicker in any step. Nothing in this check points to a faulty display.");
    } else {
        if (!evenSteady.empty())
            out.push_back("POINTS TO THE MONITOR: you saw flicker at a STEADY frame rate (step " + stepList(evenSteady) +
                          ") while this computer delivered perfectly even frames. A healthy VRR monitor should not "
                          "flicker then. Run this check on Windows too: if the same steps flicker there, the display "
                          "is most likely faulty.");
        if (!evenChanging.empty())
            out.push_back("Flicker while the frame rate CHANGED (step " + stepList(evenChanging) +
                          "), with clean timing from this computer. Small brightness shifts when the refresh rate "
                          "changes are a known VRR side effect on many monitors (especially VA panels) and are not "
                          "necessarily a defect. If Windows shows the same, it is the monitor's normal behaviour.");
        if (!uneven.empty())
            out.push_back("POINTS TO THIS COMPUTER: flicker in step " + stepList(uneven) +
                          ", where this computer delivered UNEVEN frame timing (its graphics driver / compositor did "
                          "not pace the frames cleanly). That flicker is caused on the computer side, not proof of a "
                          "faulty display. If those steps look fine on Windows, the monitor is OK.");
        if (!unknown.empty())
            out.push_back("Flicker in step " + stepList(unknown) +
                          " cannot be attributed: the system reported no display timing there.");
    }

    if (!unevenQuiet.empty())
        out.push_back("Note: this computer also delivered uneven timing in step " + stepList(unevenQuiet) +
                      ", where you did not report flicker.");
    out.push_back("Software cannot see what the panel itself does. The monitor's own refresh-rate readout (OSD) is "
                  "the final word on what it is doing.");
    return out;
}

}  // namespace vrr
