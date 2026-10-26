// Win32 window: windowed, borderless fullscreen and (via DXGI) exclusive fullscreen.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "../common/backend.h"
#include "../common/config.h"

namespace vrr {

class DxgiBackendBase;

std::string narrow(const wchar_t* w);

class WinPlatform : public Platform {
public:
    ~WinPlatform() override;
    bool create(const Config& cfg, std::string& err);
    HWND hwnd() const { return hwnd_; }
    void setBackend(DxgiBackendBase* b) { backend_ = b; }

    const char* name() const override { return "Win32"; }
    void pollEvents(std::vector<KeyEvent>& keys) override;
    bool quitRequested() const override { return quit_; }
    std::vector<WindowMode> supportedWindowModes() const override {
        return {WindowMode::Windowed, WindowMode::Borderless, WindowMode::Exclusive};
    }
    WindowMode windowMode() const override { return mode_; }
    void setWindowMode(WindowMode m) override;
    DisplayInfo displayInfo() override;

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void queryDisplay();

    HWND hwnd_ = nullptr;
    bool quit_ = false;
    WindowMode mode_ = WindowMode::Windowed;
    RECT windowedRect_{};
    DxgiBackendBase* backend_ = nullptr;
    std::vector<KeyEvent> keys_;
    bool displayDirty_ = true;
    DisplayInfo cached_;
};

}  // namespace vrr
