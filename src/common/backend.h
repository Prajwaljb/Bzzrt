// Platform / graphics backend interfaces shared by the Windows (D3D12/D3D11)
// and Linux (Vulkan WSI, DRM/KMS) implementations.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrr {

enum class WindowMode { Windowed, Borderless, Exclusive };
const char* windowModeName(WindowMode m);

enum class Key {
    None, Space, Up, Down, Left, Right, PageUp, PageDown, Plus, Minus,
    Fullscreen, V, P, G, C, H, L, R, K, LBracket, RBracket, Escape, Q,
    Y, N, S, A, Enter, Tab, Backspace,
    D0, D1, D2, D3, D4, D5, D6, D7, D8, D9  // keep contiguous: digit = key - D0
};

inline int keyDigit(Key k) {
    return (k >= Key::D0 && k <= Key::D9) ? static_cast<int>(k) - static_cast<int>(Key::D0) : -1;
}

struct KeyEvent {
    Key key = Key::None;
    bool shift = false;
};

// Patterns. Keep in sync with shaders/pattern.frag, src/win/shader_hlsl.h and patterns.cpp.
enum Pattern : int {
    kPatMidGray = 0,
    kPatDarkGray,
    kPatWhite,
    kPatBlack,
    kPatGradient,
    kPatHighContrast,
    kPatMoving,
    kPatImage,
    kPatCount
};
const char* patternName(int p);

// Shader parameter block. 64 bytes; identical layout for HLSL cbuffer and GLSL push constants.
struct ShaderParams {
    float resolution[2];
    float overlayPos[2];
    float overlaySize[2];
    float moveX;
    uint32_t pattern;
    uint32_t flags;      // kFlag* below
    uint32_t frame;
    float imageSize[2];
    float pad[4];
};
static_assert(sizeof(ShaderParams) == 64, "ShaderParams must be 64 bytes");

constexpr uint32_t kFlagOverlay = 1u << 0;
constexpr uint32_t kFlagCounter = 1u << 1;
constexpr uint32_t kFlagImage = 1u << 2;
constexpr uint32_t kFlagSrgbTarget = 1u << 3;

struct FrameParams {
    uint64_t frameIndex = 0;
    double patternTime = 0.0;  // intended presentation time (s), drives motion
    int pattern = kPatMidGray;
    bool overlayVisible = true;
    bool frameCounter = false;
    const uint32_t* overlayPixels = nullptr;  // RGBA8, kOverlayW x kOverlayH
    uint64_t overlayVersion = 0;
    int overlayX = 16, overlayY = 16;
    int overlayW = 0, overlayH = 0;  // used area (pixels outside are transparent)
};

enum class RenderResult { Rendered, Skipped, Fatal };

struct PresentOutcome {
    const char* status = "OK";  // static string
    bool ok = true;
    bool fatal = false;
};

// Set in the frame index passed to present() for a repeat of the previous frame (app-side frame doubling).
constexpr uint64_t kRepeatFrameBit = 1ull << 63;

// A presentation timestamp reported by the driver/OS for a given frame,
// expressed in the nowNs() clock domain.
struct DisplayTimingSample {
    uint64_t frameIndex = 0;
    int64_t displayNs = 0;
};

struct PresentModeInfo {
    std::string id;    // short CLI token
    std::string name;  // human readable
    bool vsync = true; // false = tearing / unsynchronized
};

// A line for the guided check's start screen about whether the setup can do VRR.
struct SetupNote {
    std::string text;
    bool warning = false;
};

struct BackendInfo {
    std::string api;
    std::string gpu;
    std::string swapFormat;
    std::string presentMode;
    std::string tearing;
    std::string timingSource;
    bool timingAvailable = false;
    int width = 0, height = 0;  // swap-chain / framebuffer size
    std::vector<std::string> notes;
    std::vector<SetupNote> setup;
};

struct DisplayInfo {
    std::string outputName;
    std::string monitorName;
    int width = 0, height = 0;
    double refreshHz = 0.0;
    std::string extra;
    // Raw EDID of the monitor, empty if the platform cannot read it.
    std::vector<uint8_t> edid;
    // A VRR range the OS reports directly (macOS), 0 = none; osRangeSource says where it came from.
    double osVrrMinHz = 0.0, osVrrMaxHz = 0.0;
    std::string osRangeSource;
    // Identity for remembering this monitor's range when there is no EDID (e.g. macOS vendor/model/serial).
    std::string monitorKey;
};

class Platform {
public:
    virtual ~Platform() = default;
    virtual const char* name() const = 0;
    virtual void pollEvents(std::vector<KeyEvent>& keys) = 0;
    virtual bool quitRequested() const = 0;
    virtual std::vector<WindowMode> supportedWindowModes() const = 0;
    virtual WindowMode windowMode() const = 0;
    virtual void setWindowMode(WindowMode m) = 0;
    virtual DisplayInfo displayInfo() = 0;
    // Low-refresh test: runs the output in a display mode of hz (exclusive fullscreen) so the VRR
    // ceiling drops to hz and the driver cannot hide low frame rates by repeating frames.
    // hz = 0 restores the normal mode. Returns false if unsupported or the switch failed.
    virtual bool canLimitRefresh() const { return false; }
    // Refresh rates the output offers at the current resolution (for picking the low-refresh ceiling).
    virtual std::vector<double> availableRefreshRates() { return {}; }
    virtual bool setRefreshCeiling(double hz) {
        (void)hz;
        return false;
    }
};

class Backend {
public:
    virtual ~Backend() = default;
    // Waits for a free frame slot, renders and submits GPU work. Does NOT present.
    virtual RenderResult renderFrame(const FrameParams& p) = 0;
    // Issues the present / page flip. The caller invokes this at the target time.
    virtual PresentOutcome present(uint64_t frameIndex) = 0;
    // Collects any driver-reported presentation timestamps that became available.
    virtual void pollTiming(std::vector<DisplayTimingSample>& out) = 0;
    virtual BackendInfo info() = 0;
    virtual std::vector<PresentModeInfo> presentModes() const = 0;
    virtual int presentModeIndex() const = 0;
    virtual void setPresentMode(int index) = 0;
    virtual bool supports10bit() const = 0;
    virtual bool is10bit() const = 0;
    virtual void set10bit(bool on) = 0;
    // Frame doubling experiment: the previous frame can be shown again by calling renderFrame() with its
    // FrameParams and present(index | kRepeatFrameBit); its timestamp is reported with the bit set.
    virtual bool canRepeatFrames() const { return false; }
};

}  // namespace vrr
