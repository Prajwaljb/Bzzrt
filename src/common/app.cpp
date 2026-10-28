#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "clock.h"
#include "edid.h"
#include "patterns.h"
#include "range_store.h"

namespace vrr {

namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

const double kDurations[] = {0, 10, 30, 60, 120, 300, 600};
constexpr int64_t kStepMinNs = 10000000000LL;

constexpr uint32_t kWhite = rgba8(235, 235, 235);
constexpr uint32_t kGray = rgba8(160, 160, 160);
constexpr uint32_t kCyan = rgba8(90, 210, 255);
constexpr uint32_t kYellow = rgba8(255, 220, 60);
constexpr uint32_t kRed = rgba8(255, 80, 70);
constexpr uint32_t kGreen = rgba8(90, 230, 110);

std::string wallClockStamp(const char* format) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::strftime(buf, sizeof buf, format, &tmv);
    return buf;
}

std::string displayModeString(const DisplayInfo& d) {
    if (d.width <= 0) return "unknown";
    return fmt("%dx%d@%.3fHz", d.width, d.height, d.refreshHz);
}

// Breaks text at spaces into lines of at most width characters (an empty string stays one empty line).
std::vector<std::string> wordWrap(const std::string& text, size_t width) {
    if (text.size() <= width) return {text};  // keeps deliberate spacing in short lines
    std::vector<std::string> out;
    std::string line;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(' ', pos);
        if (end == std::string::npos) end = text.size();
        std::string word = text.substr(pos, end - pos);
        pos = end + 1;
        while (word.size() > width) {  // a single over-long word is split hard
            if (!line.empty()) out.push_back(line), line.clear();
            out.push_back(word.substr(0, width));
            word.erase(0, width);
        }
        if (!line.empty() && line.size() + 1 + word.size() > width) out.push_back(line), line.clear();
        line += (line.empty() ? "" : " ") + word;
    }
    out.push_back(line);
    return out;
}

}  // namespace

App::App(const Config& cfg, Platform& platform, Backend& backend)
    : cfg_(cfg), platform_(platform), backend_(backend) {
    params_ = cfg.params;
    testIdx_ = std::max(0, findTest(cfg.test));
    curDef_ = allTests()[testIdx_];
    rangeAnyFromCli_ = cfg.vrrMinHz > 0 || cfg.vrrMaxHz > 0;
    duration_ = cfg.durationSec;
    logging_ = cfg.log;
    pattern_ = cfg.pattern;
    overlayVisible_ = cfg.overlay;
    counter_ = cfg.frameCounter;
    mode_ = cfg.advanced ? Mode::Advanced : Mode::Intro;
    // Brightness flicker is easiest to see on a dark uniform gray.
    if (!cfg.advanced && !cfg.patternGiven) pattern_ = kPatDarkGray;
    doubling_ = cfg.doubleFrames && backend.canRepeatFrames();
}

int App::repeatMultiple(double intervalSec) const {
    if (!doubling_) return 1;
    // Enough repeats to keep every refresh above the LFC threshold, never faster than the panel's maximum.
    const int m = static_cast<int>(std::ceil(intervalSec * lfcThreshold()));
    return std::max(1, std::min(m, static_cast<int>(std::floor(intervalSec * rangeMax()))));
}

double App::rangeMin() const { return rangeKnown() ? vrrMin_ : 48.0; }

double App::rangeMax() const {
    if (lowHz_ > 0) return lowHz_;
    double m = rangeKnown() ? vrrMax_ : (dinfo_.refreshHz > 0 ? dinfo_.refreshHz : 144.0);
    if (dinfo_.refreshHz > rangeMin()) m = std::min(m, dinfo_.refreshHz);
    return m;
}

double App::lfcThreshold() const {
    if (lfcFps_ > 0) return lfcFps_;
    return std::min(rangeMin() * 1.1, rangeMax() / 2);
}

bool App::modeLimited() const {
    return rangeKnown() && lowHz_ == 0 && dinfo_.refreshHz > vrrMin_ && dinfo_.refreshHz < vrrMax_ - 1;
}

void App::rebuildSteps() {
    steps_ = guidedSteps(rangeMin(), rangeMax(), lowHz_);
    guidedSignature_.clear();
}

void App::resolveRange(bool force) {
    const EdidInfo e = parseEdid(dinfo_.edid);
    std::string key = !dinfo_.monitorKey.empty() ? dinfo_.monitorKey : edidMonitorKey(e);
    if (key.empty()) {
        key = "name:" + (!dinfo_.monitorName.empty() ? dinfo_.monitorName : dinfo_.outputName);
        for (char& c : key)
            if (c == ' ' || c == '\t') c = '_';
    }
    if (rangeResolved_ && key == rangeKey_ && !force) return;
    rangeResolved_ = true;
    rangeKey_ = key;

    double dMin = 0, dMax = 0;
    std::string dSrc;
    if (dinfo_.osVrrMinHz > 0 && dinfo_.osVrrMaxHz > dinfo_.osVrrMinHz) {
        dMin = dinfo_.osVrrMinHz;
        dMax = dinfo_.osVrrMaxHz;
        dSrc = dinfo_.osRangeSource;
    } else {
        edidVrrRange(e, dMin, dMax, dSrc);
    }
    detMin_ = dMin;
    detMax_ = dMax;
    detSource_ = dSrc;
    haveManual_ = loadRange(key, manual_);
    applyRange();
    if (detectedKnown())
        std::printf("VRR range detected: %.0f-%.0f Hz (%s), monitor key %s\n", detMin_, detMax_, detSource_.c_str(),
                    key.c_str());
    else
        std::printf("VRR range: nothing detected for %s\n", key.c_str());
    if (haveManual_)
        std::printf("VRR range entered by you: %.0f-%.0f Hz (%s)\n", manual_.minHz, manual_.maxHz,
                    manual_.useManual ? "in use" : "not in use");
    if (rangeKnown()) std::printf("VRR range in use: %.0f-%.0f Hz (%s)\n", vrrMin_, vrrMax_, rangeSource_.c_str());
    else std::printf("VRR range unknown: the check will ask for it\n");
    std::fflush(stdout);
}

bool App::usingManual() const { return haveManual_ && (manual_.useManual || !detectedKnown()); }

void App::applyRange() {
    // The detected range and the user's entry are both kept; the user picks which one applies (A key).
    // Command-line values override either.
    const bool man = usingManual();
    const double baseMin = man ? manual_.minHz : detMin_, baseMax = man ? manual_.maxHz : detMax_;
    vrrMin_ = cfg_.vrrMinHz > 0 ? cfg_.vrrMinHz : baseMin;
    vrrMax_ = cfg_.vrrMaxHz > 0 ? cfg_.vrrMaxHz : baseMax;
    lfcFps_ = cfg_.lfcFps > 0 ? cfg_.lfcFps : man ? manual_.lfcFps : 0;
    const std::string base = man ? std::string("entered by you") : detSource_;
    if (cfg_.vrrMinHz > 0 && cfg_.vrrMaxHz > 0) rangeSource_ = "command line";
    else if (rangeAnyFromCli_) rangeSource_ = "command line + " + base;
    else rangeSource_ = base;
    if (!rangeKnown()) {
        vrrMin_ = vrrMax_ = 0;
        rangeSource_.clear();
    }
    lowStatus_.clear();
    // The low-refresh ceiling was chosen for the previous minimum; it must stay between 1.05x and 2x of the new one.
    if (lowHz_ > 0 && (!rangeKnown() || lowHz_ < vrrMin_ * 1.05 || lowHz_ > vrrMin_ * 2 - 0.5)) {
        platform_.setRefreshCeiling(0);
        platform_.setWindowMode(WindowMode::Borderless);
        lowHz_ = 0;
        lowStatus_ = "The low-refresh test was switched off: its display mode does not fit the new range.";
    }
    rebuildSteps();
}

void App::toggleRangeSource() {
    if (!haveManual_) {
        beginRangeEdit(false);
        return;
    }
    if (!detectedKnown()) {
        rangeNote_ = "Nothing was detected for this monitor, so your entry stays in use.";
        return;
    }
    manual_.useManual = !manual_.useManual;
    saveRange(rangeKey_, manual_);
    cfg_.vrrMinHz = cfg_.vrrMaxHz = cfg_.lfcFps = 0;  // an explicit choice on screen replaces command-line values
    rangeAnyFromCli_ = false;
    applyRange();
    results_.clear();
    if (mode_ == Mode::Results) mode_ = Mode::Intro;
    rangeNote_ = manual_.useManual ? "Using the range you entered." : "Using the detected range.";
    std::printf("VRR range in use: %.0f-%.0f Hz (%s)\n", vrrMin_, vrrMax_, rangeSource_.c_str());
    std::fflush(stdout);
}

void App::flash(const std::string& msg) {
    message_ = msg;
    messageUntil_ = nowNs() + 3000000000LL;
    std::printf("%s\n", msg.c_str());
    std::fflush(stdout);
}

// While a test runs its own parameters apply (guided steps override the jitter range).
std::string App::testLabel() const { return describeTest(curDef_, running_ ? sched_.params() : params_); }

void App::refreshInfo() {
    binfo_ = backend_.info();
    const double oldRefresh = dinfo_.refreshHz;
    dinfo_ = platform_.displayInfo();
    // A different monitor (window moved) gets its own range; a mode change reshapes the steps.
    if (mode_ != Mode::Step && !running_) {
        resolveRange();
        if (std::fabs(oldRefresh - dinfo_.refreshHz) > 0.5) rebuildSteps();
    }
}

void App::refreshLogState() {
    if (!log_.isOpen()) return;
    LogState s;
    s.testMode = testLabel();
    s.windowMode = windowModeName(platform_.windowMode());
    const auto modes = backend_.presentModes();
    const int pm = backend_.presentModeIndex();
    const bool vs = (pm >= 0 && pm < static_cast<int>(modes.size())) ? modes[pm].vsync : true;
    s.syncMode = vs ? "vsync/no-tearing" : "tearing/unsynchronized";
    s.presentMode = binfo_.presentMode;
    s.colorFormat = binfo_.swapFormat;
    s.displayMode = displayModeString(dinfo_);
    s.timingSource = binfo_.timingAvailable ? binfo_.timingSource : "none (CPU present timestamps only)";
    logState_ = log_.setState(s);
}

std::vector<std::string> App::headerLines() const {
    std::vector<std::string> h;
    h.push_back("VRR/LFC diagnostic log");
    h.push_back("wall_clock_start: " + wallClockStamp("%Y-%m-%d %H:%M:%S"));
    h.push_back("api: " + binfo_.api);
    h.push_back("platform: " + std::string(platform_.name()));
    h.push_back("gpu: " + binfo_.gpu);
    h.push_back("output: " + dinfo_.outputName + " " + dinfo_.monitorName);
    h.push_back("display_mode: " + displayModeString(dinfo_) + " " + dinfo_.extra);
    h.push_back("swapchain_format: " + binfo_.swapFormat);
    h.push_back("present_mode: " + binfo_.presentMode);
    h.push_back("tearing: " + binfo_.tearing);
    h.push_back("timing_source: " + (binfo_.timingAvailable ? binfo_.timingSource : std::string("none")));
    h.push_back("test: " + testLabel());
    h.push_back(fmt("seed: %llu", static_cast<unsigned long long>(params_.seed)));
    h.push_back(rangeKnown() ? fmt("vrr_range: %.1f-%.1f Hz (%s), effective max %.1f Hz, lfc_threshold %.1f FPS",
                                   vrrMin_, vrrMax_, rangeSource_.c_str(), rangeMax(), lfcThreshold())
                             : fmt("vrr_range: unknown (statistics assume %.0f-%.0f Hz)", rangeMin(), rangeMax()));
    h.push_back(std::string("pattern: ") + patternName(pattern_));
    for (const auto& n : binfo_.notes) h.push_back("note: " + n);
    h.push_back("time_s/host_monotonic_ns: CPU time when Present/flip was called. actual_frametime_ms: interval "
                "between consecutive Present calls. display_time_s: driver/OS reported presentation time "
                "(empty if unavailable). None of these prove what the panel scanned out.");
    return h;
}

std::vector<std::string> App::summaryLines() const {
    std::vector<std::string> s;
    const auto& st = stats_;
    auto fps = [](double ms) { return ms > 0 ? 1000.0 / ms : 0.0; };
    s.push_back("RESULT " + testLabel() + " -- " + lastStopReason_);
    s.push_back(fmt("frames %llu  avg %.3f ms (%.2f FPS)  1%% low %.2f FPS (%.3f ms)  0.1%% low %.2f FPS (%.3f ms)",
                    static_cast<unsigned long long>(st.frames()), st.avgMs(), fps(st.avgMs()),
                    fps(st.percentileMs(99.0)), st.percentileMs(99.0), fps(st.percentileMs(99.9)),
                    st.percentileMs(99.9)));
    s.push_back(fmt("largest spike %.3f ms (frame %llu)  late %llu  below %.0f Hz floor %llu  floor crossings %llu",
                    st.maxMs(), static_cast<unsigned long long>(st.maxFrame()),
                    static_cast<unsigned long long>(st.lateFrames()), rangeMin(),
                    static_cast<unsigned long long>(st.belowFloor()),
                    static_cast<unsigned long long>(st.floorCrossings())));
    s.push_back(fmt("present call avg %.3f ms max %.3f ms", st.presentCallAvgMs(), st.presentCallMaxMs()));
    if (st.displayCount())
        s.push_back(fmt("display-reported interval: n %llu avg %.3f ms min %.3f max %.3f (%s)",
                        static_cast<unsigned long long>(st.displayCount()), st.displayAvgMs(), st.displayMinMs(),
                        st.displayMaxMs(), binfo_.timingSource.c_str()));
    else
        s.push_back("display-reported interval: not available");
    if (!lastLogPath_.empty()) s.push_back("csv: " + lastLogPath_);
    return s;
}

void App::selectTest(int index) {
    const int n = static_cast<int>(allTests().size());
    testIdx_ = ((index % n) + n) % n;
    curDef_ = allTests()[testIdx_];
    if (running_) {
        stopTest("test changed");
        startTest(nowNs(), params_);
    }
}

void App::startTest(int64_t now, const TestParams& p) {
    sched_.start(curDef_, p);
    stats_.reset(rangeMin(), rangeMax());
    running_ = true;
    haveResults_ = true;
    testStartNs_ = now;
    testStartFrame_ = frame_ + 1;
    schedElapsed_ = 0.0;
    lastDispFrame_ = ~0ull;
    lastStopReason_ = "running";
    refreshInfo();
    lastLogPath_.clear();
    if (logging_) {
        std::string id = logId_.empty() ? curDef_.id : logId_;
        std::string path =
            cfg_.logDir + "/vrrlog_" + wallClockStamp("%Y%m%d-%H%M%S") + "_" + logPrefix_ + id + ".csv";
        if (log_.open(path, headerLines(), now)) {
            lastLogPath_ = path;
            refreshLogState();
        } else {
            flash("Cannot open CSV log " + path);
        }
    }
    flash("Started: " + testLabel() + (duration_ > 0 ? fmt(" for %.0f s", duration_) : std::string(" (until stopped)")));
}

void App::stopTest(const char* reason) {
    if (!running_) return;
    running_ = false;
    lastStopReason_ = reason;
    const auto lines = summaryLines();
    log_.close(lines);
    for (const auto& l : lines) std::printf("%s\n", l.c_str());
    std::fflush(stdout);
    flash(std::string("Test ") + reason);
}

void App::handleKey(const KeyEvent& k) {
    if (mode_ != Mode::Advanced) {
        handleGuidedKey(k);
        return;
    }
    switch (k.key) {
        case Key::Escape:
        case Key::Q: quit_ = true; break;
        case Key::Tab:
            leaveGuided(Mode::Intro);
            rebuildSteps();
            break;
        case Key::Space:
            if (running_) stopTest("stopped by user");
            else startTest(nowNs(), params_);
            break;
        case Key::Up: selectTest(testIdx_ - 1); break;
        case Key::Down: selectTest(testIdx_ + 1); break;
        case Key::PageUp: selectTest(testIdx_ - 5); break;
        case Key::PageDown: selectTest(testIdx_ + 5); break;
        case Key::Plus:
        case Key::Minus: {
            const double step = (k.shift ? 10.0 : 1.0) * (k.key == Key::Plus ? 1.0 : -1.0);
            params_.customFps = std::clamp(std::round(params_.customFps + step), 1.0, 1000.0);
            const int custom = findTest("custom");
            if (testIdx_ != custom) selectTest(custom);
            else sched_.params().customFps = params_.customFps;
            break;
        }
        case Key::Left: pattern_ = (pattern_ + kPatCount - 1) % kPatCount; break;
        case Key::Right:
        case Key::G: pattern_ = (pattern_ + 1) % kPatCount; break;
        case Key::Fullscreen: {
            const auto modes = platform_.supportedWindowModes();
            auto it = std::find(modes.begin(), modes.end(), platform_.windowMode());
            const size_t idx = it == modes.end() ? 0 : (static_cast<size_t>(it - modes.begin()) + 1) % modes.size();
            platform_.setWindowMode(modes[idx]);
            flash(std::string("Window mode: ") + windowModeName(platform_.windowMode()));
            break;
        }
        case Key::V: {
            const auto modes = backend_.presentModes();
            const int cur = backend_.presentModeIndex();
            if (cur < 0 || modes.empty()) break;
            for (size_t i = 0; i < modes.size(); ++i) {
                if (modes[i].vsync != modes[cur].vsync) {
                    backend_.setPresentMode(static_cast<int>(i));
                    flash("Present mode: " + modes[i].name);
                    break;
                }
            }
            break;
        }
        case Key::P: {
            const auto modes = backend_.presentModes();
            if (modes.empty()) break;
            const int next = (backend_.presentModeIndex() + 1) % static_cast<int>(modes.size());
            backend_.setPresentMode(next);
            flash("Present mode: " + modes[next].name);
            break;
        }
        case Key::C:
            if (backend_.supports10bit()) {
                backend_.set10bit(!backend_.is10bit());
                flash(backend_.is10bit() ? "Requested 10-bit swap chain" : "Requested 8-bit swap chain");
            } else {
                flash("10-bit output not supported by this backend/surface");
            }
            break;
        case Key::H: overlayVisible_ = !overlayVisible_; break;
        case Key::K: counter_ = !counter_; break;
        case Key::L:
            logging_ = !logging_;
            flash(logging_ ? "CSV logging ON (applies to the next test start)" : "CSV logging OFF (next test)");
            break;
        case Key::LBracket:
        case Key::RBracket: {
            const int n = static_cast<int>(sizeof kDurations / sizeof kDurations[0]);
            int idx = 0;
            for (int i = 0; i < n; ++i)
                if (kDurations[i] <= duration_ + 1e-9) idx = i;
            idx = std::clamp(idx + (k.key == Key::RBracket ? 1 : -1), 0, n - 1);
            duration_ = kDurations[idx];
            flash(duration_ > 0 ? fmt("Test duration %.0f s", duration_) : std::string("Test duration: until stopped"));
            break;
        }
        case Key::R:
            params_.seed = k.shift ? (params_.seed > 0 ? params_.seed - 1 : 0) : params_.seed + 1;
            flash(fmt("Random seed %llu (applies on next start)", static_cast<unsigned long long>(params_.seed)));
            break;
        default: break;
    }
}

void App::buildOverlay(int64_t now) {
    refreshInfo();
    refreshLogState();

    const int scale = (binfo_.width >= 1800 && binfo_.height >= 1000) ? 2 : 1;
    const int lh = 10 * scale;
    const int pad = 6 * scale;
    const size_t maxChars = static_cast<size_t>((kOverlayW - 2 * pad) / (8 * scale));
    overlay_.clear();
    struct Line {
        std::string text;
        uint32_t color;
    };
    std::vector<Line> lines;
    // Long lines wrap onto indented continuation lines.
    auto wrap = [&](std::vector<Line>& out, const std::string& t, uint32_t c) {
        size_t pos = 0;
        bool first = true;
        do {
            const std::string prefix = first ? "" : "    ";
            const size_t take = maxChars - prefix.size();
            out.push_back({prefix + t.substr(pos, take), c});
            pos += take;
            first = false;
        } while (pos < t.size());
    };
    auto add = [&](const std::string& t, uint32_t c = kWhite) { wrap(lines, t, c); };
    auto fpsOf = [](double ms) { return ms > 0 ? 1000.0 / ms : 0.0; };

    // --- system ---
    std::string state;
    if (running_) {
        const double el = static_cast<double>(now - testStartNs_) / 1e9;
        state = duration_ > 0 ? fmt("RUNNING %.1f / %.0f s", el, duration_) : fmt("RUNNING %.1f s", el);
    } else {
        state = fmt("IDLE @ %.0f FPS - SPACE starts test", cfg_.idleFps);
    }
    add("BZZRT VRR/LFC DIAGNOSTIC   [" + state + "]", running_ ? kGreen : kYellow);
    add("API: " + binfo_.api + "   (" + platform_.name() + ")", kCyan);
    add("GPU: " + binfo_.gpu, kCyan);
    {
        std::string out = "Output: " + dinfo_.outputName;
        if (!dinfo_.monitorName.empty()) out += " \"" + dinfo_.monitorName + "\"";
        if (dinfo_.width > 0)
            out += fmt("  %dx%d @ %.3f Hz (nominal)", dinfo_.width, dinfo_.height, dinfo_.refreshHz);
        add(out, kCyan);
    }
    if (!dinfo_.extra.empty()) add("        " + dinfo_.extra, kGray);
    add(fmt("Window: %s  %dx%d", windowModeName(platform_.windowMode()), binfo_.width, binfo_.height), kCyan);
    add("Swap chain format: " + binfo_.swapFormat, kCyan);
    add("Present mode: " + binfo_.presentMode, kCyan);
    add("Tearing: " + binfo_.tearing, kCyan);
    add(std::string("Present timing: ") +
            (binfo_.timingAvailable ? "AVAILABLE - " + binfo_.timingSource
                                    : "NOT AVAILABLE (" + binfo_.timingSource + ") - CPU timestamps only"),
        binfo_.timingAvailable ? kGreen : kYellow);
    for (const auto& n : binfo_.notes) add("  " + n, kGray);
    if (platform_.windowMode() == WindowMode::Windowed)
        add("! Windowed: most drivers/compositors only engage VRR for fullscreen. Press F11.", kYellow);

    // --- test ---
    const auto& st = (running_ || haveResults_) ? stats_ : live_;
    add(fmt("Test [%d/%d]: %s   seed %llu", testIdx_ + 1, static_cast<int>(allTests().size()), testLabel().c_str(),
            static_cast<unsigned long long>(params_.seed)),
        kWhite);
    if (!running_ && haveResults_) add("Showing results of last run (" + lastStopReason_ + ")", kYellow);
    add(fmt("Target: %.2f FPS (%.3f ms)  phase: %s", fpsOf(curTargetMs_), curTargetMs_, phase_));
    add(fmt("Measured: %.2f FPS (last 1 s)   frametime now %.3f ms   avg %.3f ms", live_.rollingFps(now),
            live_.lastMs(), st.avgMs()));
    add(fmt("1%% low: %.2f FPS (%.2f ms)   0.1%% low: %.2f FPS (%.2f ms)", fpsOf(st.percentileMs(99.0)),
            st.percentileMs(99.0), fpsOf(st.percentileMs(99.9)), st.percentileMs(99.9)));
    add(fmt("Largest spike: %.3f ms (frame %llu)   late presents: %llu   frames: %llu", st.maxMs(),
            static_cast<unsigned long long>(st.maxFrame()), static_cast<unsigned long long>(st.lateFrames()),
            static_cast<unsigned long long>(st.frames())));
    {
        const double pct = st.frames() ? 100.0 * static_cast<double>(st.belowFloor()) / st.frames() : 0.0;
        add(fmt("Below %.0f Hz VRR floor (>%.2f ms): %llu (%.1f%%)   floor crossings: %llu", rangeMin(),
                1000.0 / rangeMin(), static_cast<unsigned long long>(st.belowFloor()), pct,
                static_cast<unsigned long long>(st.floorCrossings())),
            st.belowFloor() ? kRed : kWhite);
    }
    add(fmt("Present call: last %.3f ms  avg %.3f  max %.3f   status: %s", live_.presentCallLastMs(),
            st.presentCallAvgMs(), st.presentCallMaxMs(), lastStatus_));
    if (st.displayCount())
        add(fmt("Driver-reported present interval: last %.3f  avg %.3f  min %.3f  max %.3f ms", st.displayLastMs(),
                st.displayAvgMs(), st.displayMinMs(), st.displayMaxMs()),
            kGreen);
    else
        add("Driver-reported present interval: n/a", kGray);
    add(fmt("Pattern: %s   Counter: %s   CSV log: %s   Duration: %s", patternName(pattern_), counter_ ? "on" : "off",
            logging_ ? "on" : "off", duration_ > 0 ? fmt("%.0f s", duration_).c_str() : "until stopped"));
    if (log_.isOpen()) add("Logging to " + log_.path(), kGray);
    else if (!lastLogPath_.empty()) add("Saved " + lastLogPath_, kGray);

    // --- histogram ---
    const int histTop = pad + static_cast<int>(lines.size()) * lh + 4 * scale;
    const int barW = 3 * scale;
    const int histH = 50 * scale;
    const int histLabelY = histTop + histH + 2 * scale;
    const int afterHist = histLabelY + lh + 2 * scale;

    std::vector<Line> tail;
    auto addTail = [&](const std::string& t, uint32_t c) { wrap(tail, t, c); };
    if (now < messageUntil_) addTail(">> " + message_, kYellow);
    addTail("WARNING: software cannot prove the monitor is in VRR or LFC. Watch the monitor's", kRed);
    addTail("OSD refresh readout (or a high-speed camera) while testing.", kRed);
    addTail("SPACE start/stop  UP/DN test  +/- fps  LEFT/RIGHT pattern  F11 window  V vsync", kGray);
    addTail("P present mode  C 8/10-bit  H overlay  K counter  L log  [ ] duration  R seed  Q quit", kGray);
    addTail("TAB back to the guided flicker check", kGray);

    const int totalH = afterHist + static_cast<int>(tail.size()) * lh + pad;
    const int totalW = std::min(kOverlayW, static_cast<int>(maxChars) * 8 * scale + 2 * pad);
    overlay_.fillRect(0, 0, totalW, std::min(totalH, kOverlayH), rgba8(0, 0, 0, 190));

    int y = pad;
    for (const auto& l : lines) {
        overlay_.text(pad, y, l.text, l.color, scale);
        y += lh;
    }

    const auto& hist = st.histogram();
    uint64_t hmax = 1;
    for (auto v : hist) hmax = std::max(hmax, v);
    const double lmax = std::log10(1.0 + static_cast<double>(hmax));
    overlay_.fillRect(pad, histTop, (FrameStats::kHistBins + 1) * barW, histH, rgba8(30, 30, 30, 220));
    for (int i = 0; i <= FrameStats::kHistBins; ++i) {
        if (!hist[i]) continue;
        const double centerMs = (i + 0.5) * FrameStats::kHistBinMs;
        uint32_t c = kGreen;
        if (i == FrameStats::kHistBins || centerMs > 1000.0 / rangeMin()) c = kRed;
        else if (centerMs < 1000.0 / rangeMax()) c = kYellow;
        const int h = std::max(1, static_cast<int>(histH * std::log10(1.0 + static_cast<double>(hist[i])) / lmax));
        overlay_.fillRect(pad + i * barW, histTop + histH - h, barW - (scale > 1 ? 1 : 0), h, c);
    }
    auto marker = [&](double ms, uint32_t c) {
        const int x = pad + static_cast<int>(ms / FrameStats::kHistBinMs * barW);
        overlay_.fillRect(x, histTop, scale, histH, c);
    };
    marker(1000.0 / rangeMax(), kYellow);
    marker(1000.0 / rangeMin(), kRed);
    const int histRight = pad + (FrameStats::kHistBins + 1) * barW + 4 * scale;
    overlay_.text(histRight, histTop, "present interval histogram", kGray, scale);
    overlay_.text(histRight, histTop + lh, "0-50 ms, 0.5 ms bins, log scale", kGray, scale);
    overlay_.text(histRight, histTop + 2 * lh, fmt("yellow: < %.2f ms (> %.0f Hz)", 1000.0 / rangeMax(), rangeMax()),
                  kYellow, scale);
    overlay_.text(histRight, histTop + 3 * lh, fmt("red: > %.2f ms (LFC zone)", 1000.0 / rangeMin()), kRed, scale);
    overlay_.text(histRight, histTop + 4 * lh,
                  rangeKnown() ? fmt("VRR range %.0f-%.0f Hz (%s)", vrrMin_, vrrMax_, rangeSource_.c_str())
                               : std::string("VRR range unknown: TAB, then R to set it"),
                  rangeKnown() ? kGray : kYellow, scale);
    overlay_.text(pad, histLabelY, "0", kGray, scale);
    overlay_.text(pad + 50 * barW, histLabelY, "25", kGray, scale);
    overlay_.text(pad + 96 * barW, histLabelY, "50+", kGray, scale);

    y = afterHist;
    for (const auto& l : tail) {
        overlay_.text(pad, y, l.text, l.color, scale);
        y += lh;
    }
    overlay_.commit(totalW, std::min(totalH, kOverlayH));
}

// ------------------------------------------------------------------ guided check

void App::requestStart() {
    if (!rangeKnown()) {
        beginRangeEdit(true);
        return;
    }
    results_.clear();
    reportPath_.clear();
    startStep(0);
}

void App::beginRangeEdit(bool thenStart) {
    editField_ = 1;
    startAfterEdit_ = thenStart;
    // Starts from the user's previous entry, else from the detected range.
    const double mn = haveManual_ ? manual_.minHz : detMin_, mx = haveManual_ ? manual_.maxHz : detMax_;
    editMin_ = mn > 0 ? fmt("%.0f", mn) : std::string();
    editMax_ = mx > 0 ? fmt("%.0f", mx) : std::string();
    editLfc_ = haveManual_ && manual_.lfcFps > 0 ? fmt("%.0f", manual_.lfcFps) : std::string();
    editError_.clear();
    guidedSignature_.clear();
}

void App::handleRangeKey(const KeyEvent& k) {
    std::string& field = editField_ == 1 ? editMin_ : editField_ == 2 ? editMax_ : editLfc_;
    editError_.clear();
    if (k.key == Key::Escape) {
        editField_ = 0;
        startAfterEdit_ = false;
    } else if (k.key == Key::Backspace) {
        if (!field.empty()) field.pop_back();
        else if (editField_ > 1) --editField_;
    } else if (keyDigit(k.key) >= 0) {
        if (field.size() < 3) field += static_cast<char>('0' + keyDigit(k.key));
    } else if (k.key == Key::Enter || k.key == Key::Space || k.key == Key::Tab) {
        const double mn = std::atof(editMin_.c_str()), mx = std::atof(editMax_.c_str());
        const double lfc = std::atof(editLfc_.c_str());
        if (editField_ == 1) {
            if (mn < 10 || mn > 500) editError_ = "Enter the lowest refresh rate of the VRR range, e.g. 48.";
            else editField_ = 2;
        } else if (editField_ == 2) {
            if (mx < mn + 5 || mx > 1000)
                editError_ = fmt("The maximum must be above the minimum (%.0f Hz), e.g. 144.", mn);
            else if (lfcPossible(mn, mx)) editField_ = 3;
            else editLfc_.clear(), editField_ = 4;
        } else if (editField_ == 3 && !editLfc_.empty() && (lfc < mn || lfc > mx / 2)) {
            editError_ = fmt("The threshold must be between %.0f and %.0f FPS (half the maximum); empty = automatic.",
                             mn, std::floor(mx / 2));
        } else {
            editField_ = 4;
        }
        if (editField_ == 4) {
            editField_ = 0;
            cfg_.vrrMinHz = cfg_.vrrMaxHz = cfg_.lfcFps = 0;  // the user's entry replaces command-line values
            rangeAnyFromCli_ = false;
            SavedRange r{mn, mx, editLfc_.empty() ? 0.0 : lfc, true};
            const std::string path = saveRange(rangeKey_, r);
            manual_ = r;
            haveManual_ = true;
            applyRange();
            rangeNote_ = path.empty() ? "Range set for this session (it could not be saved)."
                                      : "Your range is saved for this monitor in " + path;
            results_.clear();
            if (mode_ == Mode::Results) mode_ = Mode::Intro;
            std::printf("VRR range set: %.0f-%.0f Hz, LFC threshold %s\n", mn, mx,
                        r.lfcFps > 0 ? fmt("%.0f FPS", r.lfcFps).c_str() : "automatic");
            std::fflush(stdout);
            if (startAfterEdit_) {
                startAfterEdit_ = false;
                requestStart();
            }
        }
    }
    guidedSignature_.clear();
}

void App::handleGuidedKey(const KeyEvent& k) {
    if (editField_ > 0) {
        handleRangeKey(k);
        return;
    }
    switch (k.key) {
        case Key::Q: quit_ = true; break;
        case Key::Escape:
            if (mode_ == Mode::Step) leaveGuided(Mode::Intro);
            else quit_ = true;
            break;
        case Key::Tab: leaveGuided(Mode::Advanced); break;
        case Key::R:
            if (mode_ != Mode::Step) beginRangeEdit(false);
            break;
        case Key::A:
            if (mode_ != Mode::Step) toggleRangeSource();
            break;
        case Key::L:
            if (mode_ == Mode::Step || !platform_.canLimitRefresh()) break;
            if (lowHz_ > 0) {
                platform_.setRefreshCeiling(0);
                platform_.setWindowMode(WindowMode::Borderless);
                lowHz_ = 0;
                lowStatus_.clear();
            } else if (!rangeKnown()) {
                lowStatus_ = "Set the monitor's VRR range first (R).";
            } else {
                const double want = pickRefreshCeiling(vrrMin_, platform_.availableRefreshRates());
                if (want <= 0) {
                    lowStatus_ = fmt("The low-refresh test needs a display mode between %.0f and %.0f Hz at this "
                                     "resolution; this display offers none.",
                                     std::ceil(vrrMin_ * 1.1), std::floor(vrrMin_ * 2 - 1));
                } else if (platform_.setRefreshCeiling(want)) {
                    dinfo_ = platform_.displayInfo();
                    const double got = dinfo_.refreshHz > 0 ? dinfo_.refreshHz : want;
                    if (got < vrrMin_ * 1.05 || got > vrrMin_ * 2 - 0.5) {
                        platform_.setRefreshCeiling(0);
                        platform_.setWindowMode(WindowMode::Borderless);
                        lowStatus_ = fmt("Asked for a %.0f Hz mode but the display runs at %.0f Hz.", want, got);
                    } else {
                        lowHz_ = got;
                        lowStatus_.clear();
                    }
                } else {
                    lowStatus_ = fmt("Could not switch to a %.0f Hz exclusive fullscreen mode on this system.", want);
                }
            }
            // The step list differs between the two variants, so earlier results no longer apply.
            rebuildSteps();
            results_.clear();
            mode_ = Mode::Intro;
            break;
        case Key::Enter:
        case Key::Space:
            if (mode_ != Mode::Step) requestStart();
            break;
        case Key::Y:
        case Key::N:
        case Key::S:
            if (mode_ == Mode::Step) {
                answer_ = k.key == Key::Y ? Answer::Yes : k.key == Key::N ? Answer::No : Answer::Unsure;
                answerNs_ = nowNs();
            }
            break;
        case Key::Fullscreen: {
            if (lowHz_ > 0) break;  // the low-refresh test needs its exclusive low-refresh mode
            // Cycles windowed -> borderless -> exclusive (Windows) -> windowed.
            const auto modes = platform_.supportedWindowModes();
            auto it = std::find(modes.begin(), modes.end(), platform_.windowMode());
            const size_t idx = it == modes.end() ? 0 : (static_cast<size_t>(it - modes.begin()) + 1) % modes.size();
            platform_.setWindowMode(modes[idx]);
            break;
        }
        case Key::Left: pattern_ = (pattern_ + kPatCount - 1) % kPatCount; break;
        case Key::Right: pattern_ = (pattern_ + 1) % kPatCount; break;
        case Key::H: overlayVisible_ = !overlayVisible_; break;
        case Key::Backspace:
            if (mode_ == Mode::Step && !osdInput_.empty()) osdInput_.pop_back();
            break;
        default:
            if (mode_ == Mode::Step && keyDigit(k.key) >= 0 && osdInput_.size() < 3 && !steps_[stepIdx_].changing) {
                osdInput_ += static_cast<char>('0' + keyDigit(k.key));
                // Typing after answering keeps the step open a little longer.
                if (answer_ != Answer::None) answerNs_ = nowNs();
            }
            break;
    }
}

void App::startStep(int index) {
    if (steps_.empty()) return;
    const GuidedStep& step = steps_[index];
    if (running_) stopTest("completed");
    mode_ = Mode::Step;
    stepIdx_ = index;
    answer_ = Answer::None;
    osdInput_.clear();
    stepSamples_.clear();
    stepRepeats_ = stepRepeatsOk_ = 0;
    curDef_ = step.test;
    TestParams p = params_;
    if (step.jitterMax > 0) {
        p.jitterMinFps = step.jitterMin;
        p.jitterMaxFps = step.jitterMax;
    }
    logPrefix_ = fmt("check%d_", index + 1);
    logId_ = step.logId;
    stepStartNs_ = nowNs();
    startTest(stepStartNs_, p);
    logPrefix_.clear();
    logId_.clear();
}

void App::finishStep() {
    refreshInfo();
    StepResult r;
    r.verdict = analyzeTiming(stepSamples_, dinfo_.refreshHz);
    r.answer = answer_;
    r.targetFps = curDef_.kind == TestKind::Stable ? curDef_.fpsA : 0.0;
    r.osdHz = osdInput_.empty() ? 0 : std::atoi(osdInput_.c_str());
    r.latePct = stats_.frames() ? 100.0 * static_cast<double>(stats_.lateFrames()) / stats_.frames() : 0.0;
    if (r.latePct > kAppLateLimitPct) r.verdict.timing = Timing::AppLate;
    r.csvPath = lastLogPath_;
    r.multiple = r.targetFps > 0 ? repeatMultiple(1.0 / r.targetFps) : 0;
    r.repeats = stepRepeats_;
    r.repeatOkPct = stepRepeats_ ? 100.0 * stepRepeatsOk_ / stepRepeats_ : 0.0;
    stopTest("completed");
    results_.push_back(r);
    if (stepIdx_ + 1 < static_cast<int>(steps_.size())) {
        startStep(stepIdx_ + 1);
    } else {
        mode_ = Mode::Results;
        resultSteps_ = steps_;
        resultCtx_ = checkContext();
        writeReport();
    }
}

void App::leaveGuided(Mode to) {
    guidedSignature_.clear();  // the overlay may hold the other mode's content
    if (running_) stopTest("stopped by user");
    if (mode_ == Mode::Step) results_.clear();
    haveResults_ = false;
    mode_ = to;
}

CheckContext App::checkContext() const {
    CheckContext c;
    c.vrrMinHz = rangeMin();
    c.vrrMaxHz = rangeMax();
    c.monitorMaxHz = vrrMax_;
    c.refreshHz = dinfo_.refreshHz;
    c.lowRefresh = lowHz_ > 0;
    c.doubling = doubling_;
    return c;
}

void App::writeReport() {
    const auto& steps = resultSteps_;
    std::vector<std::string> lines;
    lines.push_back(std::string("Bzzrt ") + kVersion + " VRR flicker check - " + wallClockStamp("%Y-%m-%d %H:%M:%S"));
    lines.push_back("System: " + binfo_.api + " (" + platform_.name() + "), GPU: " + binfo_.gpu);
    lines.push_back("Output: " + dinfo_.outputName + " " + dinfo_.monitorName + " " + displayModeString(dinfo_));
    lines.push_back(fmt("VRR range: %.0f-%.0f Hz (%s)   Window: %s", vrrMin_, vrrMax_, rangeSource_.c_str(),
                        windowModeName(platform_.windowMode())));
    lines.push_back("Detected: " + (detectedKnown() ? fmt("%.0f-%.0f Hz (%s)", detMin_, detMax_, detSource_.c_str())
                                                    : std::string("nothing")) +
                    "   Entered by you: " +
                    (haveManual_ ? fmt("%.0f-%.0f Hz", manual_.minHz, manual_.maxHz) : std::string("none")));
    lines.push_back(fmt("Tested range: %.0f-%.0f Hz   LFC possible: %s   LFC threshold: %.0f FPS", rangeMin(),
                        rangeMax(), lfcPossible(rangeMin(), rangeMax()) ? "yes" : "no (max < 2 x min)",
                        lfcThreshold()));
    if (modeLimited())
        lines.push_back(fmt("NOTE: the display mode runs at %.0f Hz, below the monitor's %.0f Hz maximum", dinfo_.refreshHz,
                            vrrMax_));
    if (lowHz_ > 0)
        lines.push_back(fmt("LOW-REFRESH TEST: %.0f Hz display mode, frames cannot be repeated by the driver", lowHz_));
    if (doubling_)
        lines.push_back(fmt("FRAME DOUBLING EXPERIMENT: frames slower than %.0f FPS were shown several times by this "
                            "app at even intervals, as a driver with LFC would do it",
                            lfcThreshold()));
    lines.push_back("Timing source: " + (binfo_.timingAvailable ? binfo_.timingSource : std::string("none")));
    lines.push_back("");
    for (size_t i = 0; i < results_.size(); ++i) {
        const auto& r = results_[i];
        lines.push_back(fmt("%d. %-26s timing %-14s you: %s", static_cast<int>(i + 1), steps[i].title.c_str(),
                            timingLabel(r.verdict.timing), answerLabel(r.answer)));
        lines.push_back("   " + timingSentence(steps[i], r) +
                        fmt(" [%d intervals, %.0f%% on time]", r.verdict.samples, r.verdict.onTimePct));
        if (r.repeats > 0) {
            const std::string how = r.multiple > 1 ? fmt("each frame shown %dx (%.0f Hz), ", r.multiple, r.targetFps * r.multiple)
                                                   : std::string();
            lines.push_back(fmt("   App frame doubling: %s%d repeat refreshes, %.0f%% at the requested time.", how.c_str(),
                                r.repeats, r.repeatOkPct));
        }
        if (r.osdHz > 0) lines.push_back("   Monitor OSD: " + osdSentence(r.targetFps, r.osdHz, dinfo_.refreshHz, rangeMin()));
        if (!r.csvPath.empty()) lines.push_back("   log: " + r.csvPath);
    }
    lines.push_back("");
    lines.push_back("CONCLUSION");
    for (const auto& c : guidedConclusion(steps, results_, resultCtx_)) lines.push_back("- " + c);

    for (const auto& l : lines) std::printf("%s\n", l.c_str());
    std::fflush(stdout);
    if (!logging_) return;
    const std::string path = cfg_.logDir + "/vrrdiag-report-" + wallClockStamp("%Y%m%d-%H%M%S") + ".txt";
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (const auto& l : lines) std::fprintf(f, "%s\n", l.c_str());
        std::fclose(f);
        reportPath_ = path;
    }
}

void App::addRangeLines(const std::function<void(const std::string&, uint32_t)>& add) {
    if (!rangeKnown()) {
        add("VRR range: UNKNOWN - this monitor does not report it. Press R (or ENTER) and type it in. It is in the "
            "monitor's spec sheet or manual (FreeSync / Adaptive-Sync / G-SYNC Compatible range, e.g. 48-144 Hz).",
            kRed);
        return;
    }
    const bool weak = rangeSource_ == "EDID range limits";
    add(fmt("VRR range in use: %.0f-%.0f Hz (%s%s)", vrrMin_, vrrMax_, rangeSource_.c_str(),
            weak ? " - check it against the spec sheet" : ""),
        weak ? kYellow : kGreen);
    const bool cli = cfg_.vrrMinHz > 0 || cfg_.vrrMaxHz > 0;  // the command line overrides both
    const bool man = usingManual() && !cli, autoUsed = !usingManual() && !cli;
    const std::string det = detectedKnown() ? fmt("%.0f-%.0f Hz (%s)", detMin_, detMax_, detSource_.c_str())
                                            : std::string("nothing");
    const std::string mine = haveManual_ ? fmt("%.0f-%.0f Hz", manual_.minHz, manual_.maxHz) : std::string("none");
    add(fmt("   %s detected: %s", autoUsed ? ">" : " ", det.c_str()), autoUsed ? kWhite : kGray);
    add(fmt("   %s your entry: %s", man ? ">" : " ", mine.c_str()), man ? kWhite : kGray);
    if (haveManual_ && detectedKnown())
        add("   A switch between them   R edit your entry", kGray);
    else
        add(haveManual_ ? "   R edit your entry" : "   R enter your own range (e.g. from the spec sheet)", kGray);
    if (modeLimited())
        add(fmt("The display runs at %.0f Hz, so only %.0f-%.0f Hz of the range is used. Set it to %.0f Hz in the "
                "display settings for the full check.",
                dinfo_.refreshHz, vrrMin_, rangeMax(), vrrMax_),
            kYellow);
    if (lowHz_ > 0) return;
    if (lfcPossible(rangeMin(), rangeMax()))
        add(fmt("LFC possible (%.0f >= 2 x %.0f): below about %.0f FPS a good driver shows each frame several times.",
                rangeMax(), rangeMin(), lfcThreshold()),
            kGray);
    else
        add(fmt("LFC NOT possible: %.0f Hz is less than twice %.0f Hz, so frames below %.0f FPS cannot be doubled. "
                "Flicker or stutter there is expected with this range.",
                rangeMax(), rangeMin(), rangeMin()),
            kYellow);
}

void App::addEditorLines(const std::function<void(const std::string&, uint32_t)>& add) {
    auto field = [&](int idx, const char* label, const std::string& v, const char* hint) {
        const bool active = editField_ == idx;
        add(std::string(active ? "> " : "  ") + label + v + (active ? "_" : "") + (active ? hint : ""),
            active ? kYellow : kWhite);
    };
    add(startAfterEdit_ ? "Before the check: enter the monitor's VRR range."
                        : "Your own VRR range for this monitor. The detected range is kept; A switches between them.",
        kWhite);
    add("You find it in the monitor's manual or spec sheet (\"FreeSync range\", \"Adaptive-Sync 48-144 Hz\"), in "
        "AMD Software (Display tab), or in reviews of the monitor. If it only lists a maximum refresh rate, 48 Hz is "
        "the most common minimum.",
        kGray);
    add("", kWhite);
    field(1, "Minimum refresh rate (Hz): ", editMin_, "");
    field(2, "Maximum refresh rate (Hz): ", editMax_, "");
    const double mn = std::atof(editMin_.c_str()), mx = std::atof(editMax_.c_str());
    if (editField_ < 3 || lfcPossible(mn, mx))
        field(3, "LFC threshold (FPS): ", editLfc_,
              editLfc_.empty() ? "   (ENTER = automatic, 1.1 x minimum)" : "");
    add("", kWhite);
    add("The LFC threshold is the frame rate below which a driver starts showing each frame several times. Leave it "
        "empty unless you know your driver's value.",
        kGray);
    if (!editError_.empty()) add(editError_, kRed);
    add("", kWhite);
    add("Digits to type   BACKSPACE delete   ENTER next   ESC cancel", kWhite);
}

void App::buildGuidedOverlay(int64_t now) {
    refreshInfo();
    refreshLogState();
    const auto& steps = steps_;

    struct Item {
        std::string text;
        uint32_t color;
        bool title;
    };
    std::vector<Item> items;
    auto add = [&](const std::string& t, uint32_t c = kWhite) { items.push_back({t, c, false}); };
    auto title = [&](const std::string& t, uint32_t c = kWhite) { items.push_back({t, c, true}); };
    const bool windowed = platform_.windowMode() == WindowMode::Windowed;

    if (editField_ > 0) {
        title("MONITOR VRR RANGE", kGreen);
        std::string mon = "Monitor: " + dinfo_.outputName;
        if (!dinfo_.monitorName.empty()) mon += " " + dinfo_.monitorName;
        add(mon, kCyan);
        addEditorLines(add);
    } else if (mode_ == Mode::Intro) {
        title("VRR FLICKER CHECK", kGreen);
        add("Finds out whether flicker comes from the monitor or from this computer.");
        add("");
        add(fmt("You will see %d short tests, about 2 minutes in total. During each one, look at the gray "
                "screen and press:", static_cast<int>(steps.size())));
        add("   Y   I see flicker", kYellow);
        add("   N   no flicker", kYellow);
        add("   S   not sure", kYellow);
        add("");
        add("Tips: dim the room and look at the plain gray area. If your monitor's menu can show the current "
            "refresh rate, keep it open and type the number it shows during each test.", kGray);
        add("");
        std::string mon = "Monitor: " + dinfo_.outputName;
        if (!dinfo_.monitorName.empty()) mon += " " + dinfo_.monitorName;
        if (dinfo_.width > 0) mon += fmt(", %dx%d @ %.0f Hz", dinfo_.width, dinfo_.height, dinfo_.refreshHz);
        add(mon, kCyan);
        addRangeLines(add);
        if (!rangeNote_.empty()) add(rangeNote_, kGray);
        if (windowed)
            add("Window: NOT fullscreen - press F. VRR normally only works in fullscreen.", kRed);
        else
            add(std::string("Window: ") + windowModeName(platform_.windowMode()) + "   (F to change)", kGreen);
        for (const auto& n : binfo_.setup) add(n.text, n.warning ? kYellow : kGreen);
        if (binfo_.timingAvailable) add("Display timing: available - each test gets a timing verdict", kGreen);
        else add("Display timing: NOT available here - only your own observations will count", kYellow);
        if (lowHz_ > 0) {
            add(fmt("LOW-REFRESH TEST ON: display mode %.0f Hz, VRR range %.0f-%.0f Hz. The driver cannot repeat "
                    "frames, so the monitor runs near its minimum refresh rate, like with drivers without LFC.",
                    dinfo_.refreshHz, rangeMin(), lowHz_),
                kYellow);
            add("L  back to the normal test", kWhite);
        } else if (platform_.canLimitRefresh() && rangeKnown()) {
            add(fmt("L  low-refresh test: a display mode below %.0f Hz, so the monitor runs near %.0f Hz the way "
                    "drivers without LFC drive it",
                    vrrMin_ * 2, vrrMin_),
                kWhite);
        }
        if (!lowStatus_.empty()) add(lowStatus_, kRed);
        if (doubling_)
            add(fmt("FRAME DOUBLING EXPERIMENT ON: this app shows frames slower than %.0f FPS several times at even "
                    "intervals, as a driver with LFC would. If the low steps stop flickering, missing frame doubling "
                    "is the cause.",
                    lfcThreshold()),
                kYellow);
        else if (cfg_.doubleFrames)
            add("--double-frames is not available with this graphics backend; this run shows frames normally.", kRed);
        add("");
        add(rangeKnown() ? "ENTER start    TAB advanced mode    Q quit"
                         : "ENTER / R enter the VRR range    TAB advanced mode    Q quit",
            kWhite);
    } else if (mode_ == Mode::Step) {
        const GuidedStep& step = steps[stepIdx_];
        const double el = static_cast<double>(now - stepStartNs_) / 1e9;
        add(fmt("TEST %d OF %d", stepIdx_ + 1, static_cast<int>(steps.size())), kGray);
        title(step.title);
        add(step.explain, kGray);
        add("");
        // Refreshed once per second: every text change costs a redraw that competes with frame timing.
        if (now - liveStepNs_ >= 1000000000LL || liveStepNs_ < stepStartNs_) {
            liveStep_.verdict = analyzeTiming(stepSamples_, dinfo_.refreshHz);
            liveStep_.latePct =
                stats_.frames() > 50 ? 100.0 * static_cast<double>(stats_.lateFrames()) / stats_.frames() : 0.0;
            if (liveStep_.latePct > kAppLateLimitPct) liveStep_.verdict.timing = Timing::AppLate;
            liveStepNs_ = now;
        }
        StepResult live = liveStep_;
        live.targetFps = curDef_.kind == TestKind::Stable ? curDef_.fpsA : 0.0;
        if (live.verdict.timing == Timing::NoData) {
            add(binfo_.timingAvailable ? "Timing: measuring..." : "Timing: not available on this system", kGray);
        } else {
            const uint32_t c = live.verdict.timing == Timing::Even ? kGreen : kRed;
            add(std::string("Timing: ") + timingLabel(live.verdict.timing) + " - " + timingSentence(step, live), c);
        }
        if (windowed) add("Not fullscreen: VRR is probably off. Press F.", kRed);
        const int multiple = live.targetFps > 0 ? repeatMultiple(1.0 / live.targetFps) : 1;
        if (step.changing) {
            add("Your monitor's Hz reading will jump around in this test; no need to type it.", kGray);
        } else if (multiple > 1) {
            add(fmt("Frame doubling: this app shows each frame %dx, so the monitor should read %.0f Hz.", multiple,
                    live.targetFps * multiple),
                kGray);
            if (osdInput_.empty()) add("Type the Hz your monitor shows (optional): _", kWhite);
            else add("Monitor shows: " + osdInput_ + "_   " +
                         osdSentence(live.targetFps, std::atoi(osdInput_.c_str()), dinfo_.refreshHz, rangeMin()),
                     kWhite);
        } else {
            add("Monitor Hz with VRR: " + osdExpected(live.targetFps, rangeMin(), rangeMax()) +
                    " (a whole multiple of the FPS)",
                kGray);
            if (osdInput_.empty()) {
                add("Type the Hz your monitor shows (optional): _", kWhite);
            } else {
                const OsdReading o = interpretOsd(live.targetFps, std::atoi(osdInput_.c_str()), dinfo_.refreshHz, rangeMin());
                const uint32_t c = o.fit == OsdFit::Multiple                                     ? kGreen
                                   : o.fit == OsdFit::Ambiguous || o.fit == OsdFit::AtMinimum ? kYellow
                                                                                              : kRed;
                add("Monitor shows: " + osdInput_ + "_   " +
                        osdSentence(live.targetFps, std::atoi(osdInput_.c_str()), dinfo_.refreshHz, rangeMin()),
                    c);
            }
        }
        add("");
        if (answer_ == Answer::None) {
            add("Do you see flicker?    Y  yes    N  no    S  not sure", kYellow);
        } else {
            const double left = static_cast<double>(kStepMinNs) / 1e9 - el;
            add(std::string("Your answer: ") + answerLabel(answer_) +
                    (left > 0.5 ? fmt("   (measuring %.0f s more)", left) : std::string("   next test...")),
                kGreen);
            add("Press Y / N / S again to change it.", kGray);
        }
        add("");
        add("H hide text   LEFT/RIGHT pattern   ESC cancel", kGray);
    } else {
        title("RESULT", kGreen);
        add(fmt("Monitor %s, VRR range %.0f-%.0f Hz (%s)", dinfo_.monitorName.empty() ? dinfo_.outputName.c_str()
                                                                                    : dinfo_.monitorName.c_str(),
                rangeMin(), rangeMax(), rangeSource_.c_str()),
            kGray);
        add(fmt("  %-24s %-13s %-11s %s", "TEST", "TIMING", "YOU SAW", "MONITOR"), kGray);
        for (size_t i = 0; i < results_.size(); ++i) {
            const auto& r = results_[i];
            uint32_t c = kWhite;
            if (r.verdict.timing == Timing::Uneven || r.verdict.timing == Timing::FixedRefresh) c = kRed;
            if (r.verdict.timing == Timing::AppLate) c = kGray;
            if (r.answer == Answer::Yes) c = kYellow;
            const std::string osd = r.osdHz > 0 ? fmt("%d Hz", r.osdHz) : std::string("-");
            add(fmt("%d %-24s %-13s %-11s %s", static_cast<int>(i + 1), resultSteps_[i].title.c_str(),
                    timingLabel(r.verdict.timing), answerLabel(r.answer), osd.c_str()),
                c);
        }
        add("");
        for (const auto& p : guidedConclusion(resultSteps_, results_, resultCtx_)) {
            add(p);
            add("");
        }
        if (!reportPath_.empty()) add("Report saved: " + reportPath_, kGray);
        add("ENTER run again    R / A VRR range    TAB advanced mode    Q quit", kWhite);
    }

    // Lay out with the largest text size that fits on screen.
    const int screenW = std::max(320, binfo_.width), screenH = std::max(240, binfo_.height);
    std::string signature = fmt("%dx%d %d|", screenW, screenH, static_cast<int>(mode_));
    for (const auto& it : items) signature += it.text + fmt("#%08x%d|", it.color, it.title ? 1 : 0);
    if (signature == guidedSignature_) return;  // nothing changed: keep the current overlay version
    guidedSignature_ = signature;
    const int cardW = std::min(kOverlayW, screenW - 32);
    const int maxH = std::min(kOverlayH, screenH - 32);
    int scale = screenH >= 1300 ? 3 : 2;
    std::vector<std::pair<Item, int>> laid;  // wrapped line, its scale
    int pad = 0, height = 0;
    for (;; --scale) {
        laid.clear();
        pad = 8 * scale;
        height = 2 * pad;
        for (const auto& it : items) {
            const int s = it.title ? scale + 2 : scale;
            const size_t perLine = static_cast<size_t>(std::max(8, (cardW - 2 * pad) / (8 * s)));
            for (const auto& w : wordWrap(it.text, perLine)) {
                laid.push_back({{w, it.color, it.title}, s});
                height += it.text.empty() ? 6 * s : 11 * s;
            }
        }
        if (height <= maxH || scale == 1) break;
    }
    height = std::min(height, maxH);

    overlay_.clear();
    overlay_.fillRect(0, 0, cardW, height, rgba8(0, 0, 0, 215));
    int y = pad;
    for (const auto& [it, s] : laid) {
        if (y + 8 * s > height) break;
        overlay_.text(pad, y, it.text, it.color, s);
        y += it.text.empty() ? 6 * s : 11 * s;
    }
    overlay_.commit(cardW, height);
    overlayX_ = (screenW - cardW) / 2;
    // During a test the card sits at the top so most of the screen stays plain gray.
    overlayY_ = mode_ == Mode::Step ? 16 : std::max(16, (screenH - height) / 2);
}

int App::run() {
    appStartNs_ = nowNs();
    refreshInfo();
    {
        // Apply requested present mode by id.
        const auto modes = backend_.presentModes();
        if (!cfg_.presentMode.empty()) {
            bool found = false;
            for (size_t i = 0; i < modes.size(); ++i) {
                if (modes[i].id == cfg_.presentMode) {
                    backend_.setPresentMode(static_cast<int>(i));
                    found = true;
                }
            }
            if (!found) {
                std::string ids;
                for (const auto& m : modes) ids += " " + m.id;
                flash("Present mode '" + cfg_.presentMode + "' not available. Available:" + ids);
            }
        }
    }
    std::printf("API: %s | GPU: %s\n", binfo_.api.c_str(), binfo_.gpu.c_str());
    for (const auto& n : binfo_.setup) std::printf("%s %s\n", n.warning ? "Setup WARNING:" : "Setup:", n.text.c_str());
    std::printf("Note: software cannot prove VRR/LFC is active - watch the monitor OSD.\n");
    std::fflush(stdout);

    std::vector<KeyEvent> keys;
    std::vector<DisplayTimingSample> timing;
    int64_t schedTarget = nowNs();
    int64_t prevCall = 0;
    // --start begins after a short warm-up so the swap chain, fullscreen state and
    // presentation timing are established before the first logged frame.
    bool pendingAutostart = cfg_.autostart;
    const int64_t autostartAt = nowNs() + 750000000LL;

    while (!quit_ && !platform_.quitRequested()) {
        keys.clear();
        platform_.pollEvents(keys);
        for (const auto& k : keys) handleKey(k);
        if (quit_) break;
        if (pendingAutostart && nowNs() >= autostartAt) {
            pendingAutostart = false;
            if (mode_ == Mode::Advanced && !running_) startTest(nowNs(), params_);
            else if (mode_ == Mode::Intro) requestStart();
        }

        double interval;
        if (running_) {
            interval = sched_.nextInterval(schedElapsed_);
            schedElapsed_ += interval;
            phase_ = sched_.phase();
        } else {
            interval = 1.0 / cfg_.idleFps;
            phase_ = "idle";
        }
        curTargetMs_ = interval * 1e3;
        const int64_t target = schedTarget + static_cast<int64_t>(std::llround(interval * 1e9));

        // Frame doubling experiment: until this frame is due, the previous one is shown again at even intervals.
        const int multiple = haveLastFp_ ? repeatMultiple(interval) : 1;
        for (int k = 1; k < multiple; ++k) {
            const int64_t at = schedTarget + static_cast<int64_t>(std::llround(interval * 1e9 * k / multiple));
            if (backend_.renderFrame(lastFp_) != RenderResult::Rendered) break;
            if (nowNs() > at + 1000000) break;  // too late: skipped rather than delaying the next frame
            preciseSleepUntil(at);
            repeatExpectMs_ = interval * 1e3 / multiple;
            backend_.present(lastFp_.frameIndex | kRepeatFrameBit);
        }

        int64_t now = nowNs();
        // The overlay is redrawn at most every 100 ms, and only when the redraw is expected to finish before
        // this frame is due, so text updates never delay a frame (forced after 1 s at the latest).
        const bool overlayDue = lastOverlayNs_ == 0 || now - lastOverlayNs_ >= 100000000LL;
        const bool overlayFits = now + overlayCostNs_ + renderNormalNs_ + 500000 <= target ||
                                 now - lastOverlayNs_ >= 1000000000LL;
        const uint64_t overlayVersion = overlay_.version();
        const int64_t overlayStart = now;
        if (overlayDue && overlayFits) {
            if (mode_ == Mode::Advanced) buildOverlay(now);
            else buildGuidedOverlay(now);
            lastOverlayNs_ = now;
        }
        const bool overlayChanged = overlay_.version() != overlayVersion;
        const int64_t overlayBuildNs = nowNs() - overlayStart;

        ++frame_;
        FrameParams fp;
        fp.frameIndex = frame_;
        fp.patternTime = static_cast<double>(target - appStartNs_) / 1e9;
        fp.pattern = pattern_;
        fp.overlayVisible = overlayVisible_;
        fp.frameCounter = counter_;
        fp.overlayPixels = overlay_.pixels();
        fp.overlayVersion = overlay_.version();
        fp.overlayX = mode_ == Mode::Advanced ? 16 : overlayX_;
        fp.overlayY = mode_ == Mode::Advanced ? 16 : overlayY_;
        fp.overlayW = overlay_.usedW();
        fp.overlayH = overlay_.usedH();

        const int64_t renderStart = nowNs();
        const RenderResult rr = backend_.renderFrame(fp);
        // renderFrame can include waiting for the previous flip; only the part an overlay change adds counts.
        const int64_t renderNs = nowNs() - renderStart;
        if (overlayChanged) {
            const int64_t cost = overlayBuildNs + std::max<int64_t>(0, renderNs - renderNormalNs_);
            overlayCostNs_ = std::max(cost, overlayCostNs_ - overlayCostNs_ / 8);
        } else {
            renderNormalNs_ += (renderNs - renderNormalNs_) / 16;
        }
        if (rr == RenderResult::Fatal) {
            std::fprintf(stderr, "Fatal rendering error, exiting.\n");
            stopTest("aborted (render error)");
            return 1;
        }
        if (rr == RenderResult::Skipped) {
            // Minimized / zero-size / surface lost: keep the clock moving without presenting.
            preciseSleepUntil(std::max<int64_t>(target, nowNs() + 5000000));
            schedTarget = nowNs();
            prevCall = 0;
            continue;
        }

        now = nowNs();
        if (now < target) preciseSleepUntil(target);
        const int64_t tCall = nowNs();
        const PresentOutcome po = backend_.present(frame_);
        const int64_t tRet = nowNs();
        lastStatus_ = po.status;
        lastFp_ = fp;
        haveLastFp_ = po.ok;
        targetRing_[frame_ % kTargetRing] = interval * 1e3;
        if (po.fatal) {
            std::fprintf(stderr, "Fatal present error: %s\n", po.status);
            stopTest("aborted (present error)");
            return 1;
        }

        const bool late = (tCall - target) > 200000;  // > 0.2 ms after the scheduled time
        schedTarget = late ? tCall : target;
        const double ftMs = prevCall ? static_cast<double>(tCall - prevCall) / 1e6 : 0.0;
        const double callMs = static_cast<double>(tRet - tCall) / 1e6;
        prevCall = tCall;
        if (ftMs > 0) live_.addFrame(tCall, frame_, ftMs, callMs, late);

        if (running_ && frame_ > testStartFrame_ && ftMs > 0) {
            stats_.addFrame(tCall, frame_, ftMs, callMs, late);
            if (log_.isOpen()) {
                FrameRecord r;
                r.frame = frame_;
                r.hostNs = tCall;
                r.timeSec = static_cast<double>(tCall - testStartNs_) / 1e9;
                r.targetMs = interval * 1e3;
                r.actualMs = ftMs;
                r.presentCallMs = callMs;
                r.targetFps = 1.0 / interval;
                r.status = po.status;
                r.late = late;
                r.belowFloor = ftMs > 1000.0 / rangeMin() * 1.003;
                r.state = logState_;
                log_.push(r);
            }
        }

        timing.clear();
        backend_.pollTiming(timing);
        for (const auto& t : timing) {
            const int64_t sinceFlipNs = t.displayNs - lastAnyFlipNs_;
            lastAnyFlipNs_ = t.displayNs;
            if (t.frameIndex & kRepeatFrameBit) {
                // Repeats are not frames: they only count toward whether the doubling reached the screen on time.
                if (mode_ == Mode::Step && t.displayNs >= stepStartNs_ + 1000000000LL) {
                    ++stepRepeats_;
                    if (std::fabs(static_cast<double>(sinceFlipNs) / 1e6 - repeatExpectMs_) <= 1.5) ++stepRepeatsOk_;
                }
                continue;
            }
            if (running_ && t.frameIndex > testStartFrame_) {
                log_.setDisplayTime(t.frameIndex, t.displayNs);
                if (lastDispFrame_ != ~0ull && t.frameIndex == lastDispFrame_ + 1) {
                    const double ms = static_cast<double>(t.displayNs - lastDispNs_) / 1e6;
                    stats_.addDisplayInterval(ms);
                    // The first second of a step is left out: it contains the switch from the previous rate.
                    if (mode_ == Mode::Step && t.displayNs >= stepStartNs_ + 1000000000LL)
                        stepSamples_.push_back({targetRing_[t.frameIndex % kTargetRing], ms});
                }
            }
            lastDispFrame_ = t.frameIndex;
            lastDispNs_ = t.displayNs;
        }

        if (mode_ == Mode::Step && answer_ != Answer::None) {
            const int64_t t = nowNs();
            // Each step measures for at least kStepMinNs; after an answer the confirmation stays up briefly.
            if (t - stepStartNs_ >= kStepMinNs && t - answerNs_ >= 2000000000LL) finishStep();
        }
        if (mode_ == Mode::Advanced && running_ && duration_ > 0 &&
            static_cast<double>(tCall - testStartNs_) / 1e9 >= duration_) {
            stopTest("completed");
            if (cfg_.exitAfter) quit_ = true;
        }
    }
    stopTest("aborted (application closed)");
    return 0;
}

}  // namespace vrr
