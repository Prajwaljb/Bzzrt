#include "win_platform.h"

#include <setupapi.h>

#include <algorithm>
#include <cstdio>

#include "dxgi_backend_base.h"

namespace vrr {

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// GUID_DEVINTERFACE_MONITOR, defined here to avoid pulling in the DDK headers.
static const GUID kMonitorInterface = {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};

// The EDID Windows keeps in the monitor's device registry key. devicePath is the monitor's device interface path
// from DISPLAYCONFIG_TARGET_DEVICE_NAME.
static std::vector<uint8_t> readMonitorEdid(const wchar_t* devicePath) {
    std::vector<uint8_t> edid;
    if (!devicePath || !*devicePath) return edid;
    HDEVINFO set = SetupDiGetClassDevsW(&kMonitorInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return edid;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; edid.empty() && SetupDiEnumDeviceInterfaces(set, nullptr, &kMonitorInterface, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<uint8_t> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof dev;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, &dev)) continue;
        if (_wcsicmp(detail->DevicePath, devicePath) != 0) continue;
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(key, L"EDID", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size >= 128) {
            edid.resize(size);
            if (RegQueryValueExW(key, L"EDID", nullptr, &type, edid.data(), &size) != ERROR_SUCCESS) edid.clear();
            else edid.resize(size);
        }
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    return edid;
}

static Key keyFromVk(WPARAM vk) {
    switch (vk) {
        case VK_ESCAPE: return Key::Escape;
        case 'Q': return Key::Q;
        case VK_SPACE: return Key::Space;
        case VK_UP: return Key::Up;
        case VK_DOWN: return Key::Down;
        case VK_LEFT: return Key::Left;
        case VK_RIGHT: return Key::Right;
        case VK_PRIOR: return Key::PageUp;
        case VK_NEXT: return Key::PageDown;
        case VK_OEM_PLUS:
        case VK_ADD: return Key::Plus;
        case VK_OEM_MINUS:
        case VK_SUBTRACT: return Key::Minus;
        case VK_F11:
        case 'F': return Key::Fullscreen;
        case 'V': return Key::V;
        case 'P': return Key::P;
        case 'G': return Key::G;
        case 'C': return Key::C;
        case 'H': return Key::H;
        case 'L': return Key::L;
        case 'R': return Key::R;
        case 'A': return Key::A;
        case 'K': return Key::K;
        case VK_OEM_4: return Key::LBracket;
        case VK_OEM_6: return Key::RBracket;
        case 'Y': return Key::Y;
        case 'N': return Key::N;
        case 'S': return Key::S;
        case VK_RETURN: return Key::Enter;
        case VK_TAB: return Key::Tab;
        case VK_BACK: return Key::Backspace;
        default:
            if (vk >= '0' && vk <= '9') return static_cast<Key>(static_cast<int>(Key::D0) + static_cast<int>(vk - '0'));
            if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
                return static_cast<Key>(static_cast<int>(Key::D0) + static_cast<int>(vk - VK_NUMPAD0));
            return Key::None;
    }
}

WinPlatform::~WinPlatform() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool WinPlatform::create(const Config& cfg, std::string& err) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VrrDiagWindow";
    if (!RegisterClassExW(&wc)) return err = "RegisterClassExW failed", false;

    RECT r{0, 0, cfg.windowW, cfg.windowH};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const int w = r.right - r.left, h = r.bottom - r.top;
    // Open on the screen under the mouse, i.e. where the app was double-clicked, so a laptop with an
    // external monitor tests the monitor the user started it on rather than the primary screen.
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    POINT cursor{};
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (GetCursorPos(&cursor) && GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        x = mi.rcWork.left + std::max(0, static_cast<int>(mi.rcWork.right - mi.rcWork.left) - w) / 2;
        y = mi.rcWork.top + std::max(0, static_cast<int>(mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    }
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Synchora - VRR flicker check", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr,
                            nullptr, inst, this);
    if (!hwnd_) return err = "CreateWindowExW failed", false;
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    GetWindowRect(hwnd_, &windowedRect_);
    mode_ = WindowMode::Windowed;
    return true;
}

LRESULT CALLBACK WinPlatform::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        // Messages arrive before CreateWindowExW returns; handle() forwards to DefWindowProcW(hwnd_).
        static_cast<WinPlatform*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<WinPlatform*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT WinPlatform::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE: quit_ = true; return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;  // Alt+F4
            const Key k = keyFromVk(wp);
            const bool repeat = (lp & (1 << 30)) != 0;
            if (k != Key::None && (!repeat || k == Key::Plus || k == Key::Minus))
                keys_.push_back({k, (GetKeyState(VK_SHIFT) & 0x8000) != 0});
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && mode_ != WindowMode::Windowed) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
        case WM_MOVE:
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED: displayDirty_ = true; break;
        default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void WinPlatform::pollEvents(std::vector<KeyEvent>& keys) {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) quit_ = true;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    keys.insert(keys.end(), keys_.begin(), keys_.end());
    keys_.clear();
}

void WinPlatform::setWindowMode(WindowMode m) {
    if (m == mode_) return;
    if (mode_ == WindowMode::Exclusive && backend_) backend_->setExclusiveFullscreen(false);
    if (mode_ == WindowMode::Windowed) GetWindowRect(hwnd_, &windowedRect_);

    if (m == WindowMode::Windowed) {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hwnd_, HWND_NOTOPMOST, windowedRect_.left, windowedRect_.top,
                     windowedRect_.right - windowedRect_.left, windowedRect_.bottom - windowedRect_.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        // Borderless: a popup exactly covering the monitor, which allows DWM independent flip.
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    mode_ = m;
    if (m == WindowMode::Exclusive && (!backend_ || !backend_->setExclusiveFullscreen(true))) {
        std::printf("Exclusive fullscreen failed; using borderless fullscreen.\n");
        mode_ = WindowMode::Borderless;
    }
    displayDirty_ = true;
}

void WinPlatform::queryDisplay() {
    cached_ = DisplayInfo{};
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) return;
    cached_.outputName = narrow(mi.szDevice);
    cached_.width = mi.rcMonitor.right - mi.rcMonitor.left;
    cached_.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        cached_.refreshHz = dm.dmDisplayFrequency;
        cached_.width = static_cast<int>(dm.dmPelsWidth);
        cached_.height = static_cast<int>(dm.dmPelsHeight);
    }

    // QueryDisplayConfig gives the exact rational refresh rate, the monitor name and the link bit depth.
    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) != ERROR_SUCCESS) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(), &numModes, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return;
    for (UINT32 i = 0; i < numPaths; ++i) {
        const DISPLAYCONFIG_PATH_INFO& p = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;

        const DISPLAYCONFIG_RATIONAL& rr = p.targetInfo.refreshRate;
        if (rr.Denominator) cached_.refreshHz = static_cast<double>(rr.Numerator) / rr.Denominator;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof tn;
        tn.header.adapterId = p.targetInfo.adapterId;
        tn.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS) {
            cached_.monitorName = narrow(tn.monitorFriendlyDeviceName);
            cached_.edid = readMonitorEdid(tn.monitorDevicePath);
        }

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ci{};
        ci.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        ci.header.size = sizeof ci;
        ci.header.adapterId = p.targetInfo.adapterId;
        ci.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&ci.header) == ERROR_SUCCESS) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "Windows link color depth: %u bpc; HDR/advanced color: %s",
                          static_cast<unsigned>(ci.bitsPerColorChannel), (ci.value & 0x2) ? "ON" : "off");
            cached_.extra = buf;
        }
        break;
    }
}

bool WinPlatform::setRefreshCeiling(double hz) {
    if (!backend_) return false;
    backend_->setExclusiveRefresh(hz);
    // Re-enter exclusive fullscreen so the new display mode takes effect.
    if (mode_ == WindowMode::Exclusive) setWindowMode(WindowMode::Borderless);
    if (hz <= 0) return true;
    setWindowMode(WindowMode::Exclusive);
    if (mode_ != WindowMode::Exclusive) {
        backend_->setExclusiveRefresh(0);
        return false;
    }
    return true;
}

std::vector<double> WinPlatform::availableRefreshRates() {
    std::vector<double> hz;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) return hz;
    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) return hz;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); ++i) {
        if (dm.dmPelsWidth != cur.dmPelsWidth || dm.dmPelsHeight != cur.dmPelsHeight || dm.dmDisplayFrequency <= 1)
            continue;
        const double f = dm.dmDisplayFrequency;
        if (std::find(hz.begin(), hz.end(), f) == hz.end()) hz.push_back(f);
    }
    std::sort(hz.begin(), hz.end());
    return hz;
}

DisplayInfo WinPlatform::displayInfo() {
    if (displayDirty_) {
        queryDisplay();
        displayDirty_ = false;
    }
    return cached_;
}

}  // namespace vrr
