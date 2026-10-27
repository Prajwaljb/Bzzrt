// macOS (Cocoa) window hosting a CAMetalLayer: windowed and native fullscreen.
#pragma once

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <string>
#include <vector>

#include "../common/backend.h"
#include "../common/config.h"

@class VrrView;

namespace vrr {

class MacPlatform : public Platform {
public:
    ~MacPlatform() override;
    bool create(const Config& cfg, std::string& err);
    CAMetalLayer* layer() const;
    // VRR range macOS reports for the window's screen (macOS 12+), 0 if the display runs at a fixed rate.
    void screenVrrRange(double& minHz, double& maxHz) const;

    const char* name() const override { return "macOS"; }
    void pollEvents(std::vector<KeyEvent>& keys) override;
    bool quitRequested() const override;
    std::vector<WindowMode> supportedWindowModes() const override {
        return {WindowMode::Windowed, WindowMode::Borderless};
    }
    WindowMode windowMode() const override;
    void setWindowMode(WindowMode m) override;
    DisplayInfo displayInfo() override;

private:
    NSWindow* window_ = nil;
    VrrView* view_ = nil;
    id activity_ = nil;
};

}  // namespace vrr
