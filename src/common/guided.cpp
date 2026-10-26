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

const std::vector<GuidedStep>& guidedSteps(bool lowRefresh) {
    // With a 60 Hz ceiling, frames below 48 FPS cannot be shown twice (2 x 47 = 94 Hz), so the
    // monitor runs at the bottom of its range: the situation in which the Linux driver flickered.
    static const std::vector<GuidedStep> low = {
        {"stable55", "55 FPS, steady", "Near the top of the reduced 48-60 Hz range.", false},
        {"stable50", "50 FPS, steady", "Just above the 48 Hz minimum.", false},
        {"stable47", "47 FPS, steady",
         "Just below the minimum. At a 60 Hz ceiling frames cannot be repeated: the monitor either runs at its "
         "lowest refresh rate or the driver switches to a fixed 60 Hz.", false},
        {"stable45", "45 FPS, steady", "Below the minimum, frames cannot be repeated at a 60 Hz ceiling.", false},
        {"stable30", "30 FPS, steady", "Each frame can be shown exactly twice (60 Hz).", false},
        {"switch47-50", "Switching 50 <-> 47 FPS", "Crosses the 48 Hz minimum every second.", true},
        {"jitter45-55", "Changing 45-55 FPS", "Random frame rate around the 48 Hz minimum.", true, 45, 55},
    };
    if (lowRefresh) return low;
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

OsdReading interpretOsd(double fps, int osdHz, double refreshHz, double vrrMinHz) {
    OsdReading r;
    if (fps <= 0 || osdHz <= 0) return r;
    const double h = osdHz;
    const int k = static_cast<int>(std::lround(h / fps));
    // OSDs round to whole Hz, so allow a little slack (48 vs 50 Hz must still count as different).
    const bool multiple = k >= 1 && std::fabs(h - k * fps) <= std::max(1.5, 0.01 * h);
    const bool fixed = refreshHz > 0 && std::fabs(h - refreshHz) <= 2.0;
    // Below the minimum the panel cannot follow the frame rate, so a reading at the minimum is the panel holding
    // frames there, even when it is within rounding of the frame rate itself (47 FPS vs 48 Hz).
    // Above the minimum, a reading at the minimum means frames arrive later than the panel can wait.
    const bool atMin = fps < vrrMinHz ? std::fabs(h - vrrMinHz) <= 3.0
                                      : fps > vrrMinHz + 1.0 && std::fabs(h - vrrMinHz) <= 1.5;
    if (atMin && !fixed) {
        r.fit = OsdFit::AtMinimum;
    } else if (multiple) {
        r.fit = fixed ? OsdFit::Ambiguous : OsdFit::Multiple;
        r.multiple = k;
    } else if (fixed) {
        r.fit = OsdFit::FixedRefresh;
    } else {
        r.fit = OsdFit::Mismatch;
    }
    return r;
}

std::string osdExpected(double fps, double vrrMinHz, double vrrMaxHz) {
    std::vector<int> hz;
    for (int k = 1; k * fps <= vrrMaxHz + 0.5; ++k)
        if (k * fps >= vrrMinHz - 0.5) hz.push_back(static_cast<int>(std::lround(k * fps)));
    if (hz.empty()) return "about " + std::to_string(static_cast<int>(std::lround(vrrMinHz))) +
                           " Hz (no multiple fits, so the monitor sits at its minimum)";
    std::string s;
    for (size_t i = 0; i < hz.size(); ++i) {
        if (i) s += (i + 1 == hz.size()) ? " or " : ", ";
        s += std::to_string(hz[i]);
    }
    return s + " Hz";
}

std::string osdSentence(double fps, int osdHz, double refreshHz, double vrrMinHz) {
    const OsdReading r = interpretOsd(fps, osdHz, refreshHz, vrrMinHz);
    char buf[256];
    switch (r.fit) {
        case OsdFit::Multiple:
            if (r.multiple == 1)
                std::snprintf(buf, sizeof buf, "%d Hz = the frame rate: VRR follows the frames exactly.", osdHz);
            else
                std::snprintf(buf, sizeof buf,
                              "%d Hz = %d x %.0f FPS: VRR is working. Each frame is shown %d times.", osdHz,
                              r.multiple, fps, r.multiple);
            break;
        case OsdFit::Ambiguous:
            std::snprintf(buf, sizeof buf,
                          "%d Hz = %d x %.0f FPS, but also the fixed refresh rate, so this test cannot tell.", osdHz,
                          r.multiple, fps);
            break;
        case OsdFit::FixedRefresh:
            std::snprintf(buf, sizeof buf, "%d Hz is the fixed refresh rate, not a multiple of %.0f FPS: VRR is OFF.",
                          osdHz, fps);
            break;
        case OsdFit::AtMinimum:
            if (fps < vrrMinHz)
                std::snprintf(buf, sizeof buf,
                              "%d Hz = the monitor's minimum: frames below it are held at the lowest refresh rate.",
                              osdHz);
            else
                std::snprintf(buf, sizeof buf,
                              "%d Hz = the monitor's minimum although %.0f FPS is above it: frames arrive later than "
                              "the panel can wait, so it refreshes on its own in between.",
                              osdHz, fps);
            break;
        case OsdFit::Mismatch: {
            const double frameMs = 1000.0 / fps;
            const double holdMs = vrrMinHz > 0 ? 1000.0 / vrrMinHz : 0.0;
            const double restMs = frameMs - holdMs;
            if (fps < vrrMinHz && restMs > 0.5 && std::fabs(1000.0 / restMs - osdHz) <= std::max(3.0, 0.05 * osdHz))
                // The panel waited as long as it can, refreshed on its own, then the next frame came: no LFC.
                std::snprintf(buf, sizeof buf,
                              "%d Hz: each %.1f ms frame is split into a %.1f ms and a %.1f ms refresh (%.0f and %d Hz) "
                              "instead of being shown twice evenly. No proper frame doubling.",
                              osdHz, frameMs, holdMs, restMs, vrrMinHz, osdHz);
            else if (osdHz > fps)
                std::snprintf(buf, sizeof buf,
                              "%d Hz does not fit %.0f FPS: some refreshes are only ~%.1f ms long, so the panel "
                              "refreshes unevenly.",
                              osdHz, fps, 1000.0 / osdHz);
            else
                std::snprintf(buf, sizeof buf, "%d Hz does not fit %.0f FPS (not a whole multiple of it).", osdHz, fps);
            break;
        }
        default: return "";
    }
    return buf;
}

const char* timingLabel(Timing t) {
    switch (t) {
        case Timing::Even: return "EVEN";
        case Timing::Uneven: return "UNEVEN";
        case Timing::FixedRefresh: return "FIXED REFRESH";
        case Timing::AppLate: return "APP LATE";
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
        case Timing::AppLate:
            return fmt("The app itself missed %.0f%% of its frame times, so this test says nothing about the driver.",
                       r.latePct);
        case Timing::Uneven:
            if (!step.changing && v.commonB > 0)
                return fmt("Frames alternated between ~%.0f and ~%.0f ms instead of an even %.1f ms.", v.commonA,
                           v.commonB, targetMs);
            return fmt("Frames reached the screen unevenly (only %.0f%% on time).", v.onTimePct);
    }
    return "";
}

namespace {

// The frame doubling experiment asks one question: does the flicker below the minimum go away when every frame
// is shown twice at even intervals, as a driver with LFC would do it?
std::vector<std::string> doublingConclusion(const std::vector<StepResult>& results, double refreshHz,
                                            double vrrMinHz) {
    const auto& steps = guidedSteps(false);
    std::vector<std::string> out;
    std::vector<int> appLate, notDoubled, osdOff, clean, flicker, changingFlicker, inRangeFlicker;
    int answered = 0;
    for (size_t i = 0; i < results.size() && i < steps.size(); ++i) {
        const auto& r = results[i];
        const int idx = static_cast<int>(i);
        if (r.verdict.timing == Timing::AppLate) {
            appLate.push_back(idx);
            continue;
        }
        if (r.answer == Answer::Yes || r.answer == Answer::No) ++answered;
        if (r.repeats == 0) {
            if (r.answer == Answer::Yes) inRangeFlicker.push_back(idx);
            continue;
        }
        // The repeats only test the idea if they reached the screen between evenly paced frames.
        if (r.repeatOkPct < 90.0 || r.verdict.timing != Timing::Even) {
            notDoubled.push_back(idx);
            continue;
        }
        if (!steps[i].changing && r.osdHz > 0) {
            const OsdReading o = interpretOsd(r.targetFps, r.osdHz, refreshHz, vrrMinHz);
            if (o.fit != OsdFit::Multiple || o.multiple != r.multiple) osdOff.push_back(idx);
        }
        if (r.answer == Answer::Yes) (steps[i].changing ? changingFlicker : flicker).push_back(idx);
        else if (r.answer == Answer::No) clean.push_back(idx);
    }

    if (!appLate.empty())
        out.push_back("NOT COUNTED: in step " + stepList(appLate) + " the app itself could not keep up and missed its "
                      "own frame times, so those results say nothing about the monitor or the driver.");
    if (!notDoubled.empty())
        out.push_back("DOUBLING DID NOT WORK in step " + stepList(notDoubled) + ": the repeated frames or the frames "
                      "themselves did not reach the screen at the requested times, so those steps do not test the idea.");
    if (!osdOff.empty())
        out.push_back("In step " + stepList(osdOff) + " the monitor's reading did not match the doubled refresh rate, so "
                      "the extra refreshes may not have reached the panel there.");

    if (answered == 0) {
        out.push_back("No answers were recorded, so there is nothing to conclude about flicker.");
    } else if (!flicker.empty()) {
        out.push_back("STILL FLICKERS WITH FRAME DOUBLING: step " + stepList(flicker) + " flickered although every frame "
                      "was shown several times at even intervals, keeping the monitor well inside its range. Missing "
                      "frame doubling does not explain that flicker; it points to the monitor itself.");
        if (!clean.empty())
            out.push_back("Doubling did remove the flicker in step " + stepList(clean) + ".");
    } else if (!clean.empty()) {
        out.push_back("FRAME DOUBLING REMOVES THE FLICKER: with every frame shown twice at even intervals, step " +
                      stepList(clean) + " showed no flicker. The monitor handles these frame rates fine when frames are "
                      "doubled. If the same steps flicker in a normal run (without --double-frames), the cause is the "
                      "missing frame doubling (LFC) in this system's graphics driver, not the monitor.");
    }
    if (!changingFlicker.empty())
        out.push_back("Flicker while the frame rate changed (step " + stepList(changingFlicker) + "), even with "
                      "doubling. Small brightness shifts when the refresh rate changes are a known VRR side effect on "
                      "many monitors (especially VA panels) and not necessarily a defect.");
    if (!inRangeFlicker.empty())
        out.push_back("Flicker in step " + stepList(inRangeFlicker) + ", which is inside the VRR range and needed no "
                      "doubling.");
    out.push_back("Software cannot see what the panel itself does. The monitor's own refresh-rate readout (OSD) is "
                  "the final word on what it is doing.");
    return out;
}

}  // namespace

std::vector<std::string> guidedConclusion(const std::vector<StepResult>& results, double vrrMinHz, double refreshHz,
                                          bool lowRefresh, bool doubling) {
    if (doubling && !lowRefresh) return doublingConclusion(results, refreshHz, vrrMinHz);
    const auto& steps = guidedSteps(lowRefresh);
    std::vector<std::string> out;
    std::vector<int> evenSteady, evenChanging, uneven, unknown, noVrr, unevenQuiet;
    std::vector<int> osdMultiplied, osdFixed, osdMismatch, noLfc, appLate;
    int answered = 0, vrrProof = 0, noData = 0, osdProof = 0;
    const double period = refreshHz > 0 ? 1000.0 / refreshHz : 0.0;

    for (size_t i = 0; i < results.size() && i < steps.size(); ++i) {
        const auto& r = results[i];
        const Timing t = r.verdict.timing;
        if (t == Timing::AppLate) {
            appLate.push_back(static_cast<int>(i));
            continue;  // neither the timing nor the flicker answer can be attributed
        }
        if (t == Timing::NoData) ++noData;
        if (t == Timing::Uneven && r.answer != Answer::Yes) unevenQuiet.push_back(static_cast<int>(i));
        // A steady in-range rate shown at even intervals that are not a multiple of the fixed refresh
        // period can only happen with variable refresh.
        if (!steps[i].changing && r.targetFps >= vrrMinHz) {
            const double ms = 1000.0 / r.targetFps;
            const bool multiple = period > 0 && std::fabs(ms - std::round(ms / period) * period) < 0.7;
            if (t == Timing::Even && !multiple) ++vrrProof;
            // Uneven is not the same as fixed: only quantization to the fixed refresh period means VRR was off.
            if (t == Timing::FixedRefresh) noVrr.push_back(static_cast<int>(i));
        }
        if (!steps[i].changing && r.osdHz > 0) {
            const OsdReading o = interpretOsd(r.targetFps, r.osdHz, refreshHz, vrrMinHz);
            // Any steady reading away from the fixed refresh rate shows the panel's refresh rate is varying.
            if (o.fit == OsdFit::Multiple || o.fit == OsdFit::AtMinimum || o.fit == OsdFit::Mismatch) ++osdProof;
            if (o.fit == OsdFit::Multiple && o.multiple > 1) osdMultiplied.push_back(static_cast<int>(i));
            if (o.fit == OsdFit::FixedRefresh) osdFixed.push_back(static_cast<int>(i));
            // Readings above the frame rate are explained (uneven or split refreshes); lower ones are not.
            if (o.fit == OsdFit::Mismatch && r.osdHz < r.targetFps) osdMismatch.push_back(static_cast<int>(i));
        }
        // Even frames from the app do not mean even refreshes when the panel cannot follow them: below the minimum
        // (or when its readout is irregular) the driver must repeat frames, and the readout shows whether it did.
        bool panelIrregular = false;
        if (!steps[i].changing && r.targetFps > 0) {
            const OsdFit o = interpretOsd(r.targetFps, r.osdHz, refreshHz, vrrMinHz).fit;
            panelIrregular = o == OsdFit::Mismatch || o == OsdFit::AtMinimum;
            if (r.targetFps < vrrMinHz &&
                (panelIrregular || (o == OsdFit::None && t == Timing::Uneven)))
                noLfc.push_back(static_cast<int>(i));
        }
        if (r.answer != Answer::None && r.answer != Answer::Unsure) ++answered;
        if (r.answer != Answer::Yes) continue;
        if (t == Timing::Even && panelIrregular) uneven.push_back(static_cast<int>(i));
        else if (t == Timing::Even) (steps[i].changing ? evenChanging : evenSteady).push_back(static_cast<int>(i));
        else if (t == Timing::NoData) unknown.push_back(static_cast<int>(i));
        else uneven.push_back(static_cast<int>(i));
    }

    if (!appLate.empty())
        out.push_back("NOT COUNTED: in step " + stepList(appLate) + " the app itself could not keep up and missed its "
                      "own frame times, so those results say nothing about the monitor or the driver. Please report "
                      "this; it is a problem in this tool.");

    // 1. Was VRR actually in use?
    if (vrrProof == 0 && osdProof > 0 && osdFixed.empty()) {
        out.push_back("VRR WAS ACTIVE according to your monitor's readout: its refresh rate changed with the frame "
                      "rate instead of staying at the fixed refresh rate.");
    } else if (noData == static_cast<int>(results.size())) {
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

    if (!osdMultiplied.empty())
        out.push_back("FRAME MULTIPLICATION (step " + stepList(osdMultiplied) +
                      "): the monitor showed a whole multiple of the frame rate, e.g. 180 Hz at 90 FPS. The driver "
                      "shows each frame several times to keep the refresh rate high. This is VRR working; the Hz "
                      "reading is not supposed to equal the FPS, and it is not a fault.");
    if (!noLfc.empty() && !lowRefresh)
        out.push_back("NO PROPER FRAME DOUBLING below the VRR minimum (step " + stepList(noLfc) +
                      "): instead of showing each frame twice at even intervals, the panel waited as long as it can "
                      "and then refreshed on its own, so the refresh rate swung frame to frame (the OSD jumps). Those "
                      "swings make many monitors flicker. The cause is this system's graphics driver / compositor, "
                      "not the monitor. Workaround: keep the frame rate comfortably above the minimum, e.g. an FPS "
                      "cap of 60.");
    if (!osdFixed.empty() && !lowRefresh)
        out.push_back("Your monitor stayed at its fixed refresh rate in step " + stepList(osdFixed) +
                      ", so VRR was not active there.");
    if (!osdMismatch.empty())
        out.push_back("The monitor's reading in step " + stepList(osdMismatch) +
                      " did not fit the frame rate. Some monitors average or delay this readout; if it keeps "
                      "happening at a steady frame rate, note it when comparing with the other system.");

    // 2. Where did flicker appear, and was the computer's timing clean there?
    if (lowRefresh) {
        // Uneven timing near the floor is the point of this test, so flicker is read against the test, not the timing.
        // What the driver did below the minimum is measured, not assumed: it may hold frames at the minimum or
        // switch VRR off and run at the fixed refresh rate instead.
        std::vector<int> flicker, belowFixed, belowMin;
        double lowestEven = 0;
        for (size_t i = 0; i < results.size() && i < steps.size(); ++i) {
            const auto& r = results[i];
            if (r.answer == Answer::Yes) flicker.push_back(static_cast<int>(i));
            if (steps[i].changing || r.targetFps <= 0) continue;
            const OsdFit osd = interpretOsd(r.targetFps, r.osdHz, refreshHz, vrrMinHz).fit;
            if (r.targetFps < vrrMinHz) {
                if (r.verdict.timing == Timing::FixedRefresh || osd == OsdFit::FixedRefresh)
                    belowFixed.push_back(static_cast<int>(i));
                else if (osd == OsdFit::AtMinimum || r.verdict.timing == Timing::Uneven)
                    belowMin.push_back(static_cast<int>(i));
            } else if (r.verdict.timing == Timing::Even && r.answer != Answer::Yes &&
                       (lowestEven == 0 || r.targetFps < lowestEven)) {
                lowestEven = r.targetFps;
            }
        }
        if (lowestEven > 0)
            out.push_back(fmt("The monitor ran a steady %.0f Hz, close to its minimum, without flicker.", lowestEven));
        if (!belowFixed.empty())
            out.push_back("Below the minimum the driver switched VRR off and ran the monitor at a fixed refresh rate "
                          "(step " + stepList(belowFixed) + "), so the monitor never dropped to its minimum the way it "
                          "does on Linux. This system cannot recreate the Linux condition; the Linux flicker most likely comes from "
                          "a refresh pattern this driver never produces.");
        if (answered == 0 && flicker.empty())
            out.push_back("No answers were recorded, so there is nothing to conclude about flicker.");
        else if (flicker.empty() && !belowMin.empty())
            out.push_back("LOW-REFRESH TEST: no flicker, even with the monitor held at its minimum refresh rate (step " +
                          stepList(belowMin) + "), the situation in which Linux flickers. The monitor copes with low "
                          "refresh rates, so the Linux flicker most likely comes from how Linux drives it.");
        else if (flicker.empty())
            out.push_back("LOW-REFRESH TEST: no flicker in any step. Nothing here points to a faulty display.");
        else
            out.push_back("LOW-REFRESH TEST: flicker on this system too when the monitor runs near its minimum "
                          "refresh rate (step " + stepList(flicker) + "). So the flicker is how this monitor behaves at "
                          "very low refresh rates, not a Linux bug. Linux shows it more often because its driver lets "
                          "the refresh rate drop that low, while Windows normally repeats frames to keep it high. "
                          "Brightness flicker near the bottom of the VRR range is common (especially on VA panels); "
                          "compare with another unit or reviews of this model before calling it a fault.");
    } else if (answered == 0 && evenSteady.empty() && evenChanging.empty() && uneven.empty() && unknown.empty()) {
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
                          ", where the frames or the panel's refreshes were UNEVEN (the graphics driver / compositor "
                          "did not pace them cleanly). That flicker is caused on the computer side, not proof of a "
                          "faulty display. If those steps look fine on Windows, the monitor is OK.");
        if (!unknown.empty())
            out.push_back("Flicker in step " + stepList(unknown) +
                          " cannot be attributed: the system reported no display timing there.");
    }

    if (!unevenQuiet.empty() && !lowRefresh)
        out.push_back("Note: this computer also delivered uneven timing in step " + stepList(unevenQuiet) +
                      ", where you did not report flicker.");
    out.push_back("Software cannot see what the panel itself does. The monitor's own refresh-rate readout (OSD) is "
                  "the final word on what it is doing.");
    return out;
}

}  // namespace vrr
