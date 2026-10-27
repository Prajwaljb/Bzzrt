#include "mac_platform.h"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using vrr::Key;
using vrr::KeyEvent;

// Content view backed by a CAMetalLayer; collects key presses for the app's own event loop.
@interface VrrView : NSView
@property(nonatomic) std::vector<KeyEvent>* keys;
@property(nonatomic) bool quit;
@end

@implementation VrrView

- (BOOL)wantsUpdateLayer {
    return YES;
}

- (CALayer*)makeBackingLayer {
    CAMetalLayer* l = [CAMetalLayer layer];
    return l;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)updateDrawableSize {
    CAMetalLayer* l = (CAMetalLayer*)self.layer;
    const CGFloat scale = self.window ? self.window.backingScaleFactor : 1.0;
    l.contentsScale = scale;
    const NSSize s = self.bounds.size;
    l.drawableSize = CGSizeMake(std::max<CGFloat>(1, s.width * scale), std::max<CGFloat>(1, s.height * scale));
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

static Key keyFromEvent(NSEvent* e) {
    switch (e.keyCode) {
        case 53: return Key::Escape;
        case 36:
        case 76: return Key::Enter;
        case 48: return Key::Tab;
        case 49: return Key::Space;
        case 51: return Key::Backspace;
        case 123: return Key::Left;
        case 124: return Key::Right;
        case 125: return Key::Down;
        case 126: return Key::Up;
        case 116: return Key::PageUp;
        case 121: return Key::PageDown;
        case 103: return Key::Fullscreen;  // F11
        default: break;
    }
    NSString* chars = e.charactersIgnoringModifiers.lowercaseString;
    if (chars.length == 0) return Key::None;
    const unichar c = [chars characterAtIndex:0];
    if (c >= '0' && c <= '9') return static_cast<Key>(static_cast<int>(Key::D0) + (c - '0'));
    switch (c) {
        case 'q': return Key::Q;
        case 'f': return Key::Fullscreen;
        case 'v': return Key::V;
        case 'p': return Key::P;
        case 'g': return Key::G;
        case 'c': return Key::C;
        case 'h': return Key::H;
        case 'l': return Key::L;
        case 'r': return Key::R;
        case 'a': return Key::A;
        case 'k': return Key::K;
        case 'y': return Key::Y;
        case 'n': return Key::N;
        case 's': return Key::S;
        case '[': return Key::LBracket;
        case ']': return Key::RBracket;
        case '+':
        case '=': return Key::Plus;
        case '-':
        case '_': return Key::Minus;
        default: return Key::None;
    }
}

- (void)keyDown:(NSEvent*)e {
    const Key k = keyFromEvent(e);
    if ((e.modifierFlags & NSEventModifierFlagCommand) && k == Key::Q) {
        self.quit = true;
        return;
    }
    if (k == Key::None || !self.keys) return;
    if (e.isARepeat && k != Key::Plus && k != Key::Minus) return;
    self.keys->push_back({k, (e.modifierFlags & NSEventModifierFlagShift) != 0});
}

@end

@interface VrrWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, weak) VrrView* view;
@end

@implementation VrrWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    self.view.quit = true;
    return NO;
}
@end

namespace vrr {

namespace {
std::vector<KeyEvent> g_keys;
VrrWindowDelegate* g_delegate = nil;

NSScreen* screenOf(NSWindow* w) { return w.screen ? w.screen : NSScreen.mainScreen; }

CGDirectDisplayID displayIdOf(NSScreen* s) {
    NSNumber* n = s.deviceDescription[@"NSScreenNumber"];
    return n ? static_cast<CGDirectDisplayID>(n.unsignedIntValue) : CGMainDisplayID();
}
}  // namespace

MacPlatform::~MacPlatform() {
    if (activity_) [[NSProcessInfo processInfo] endActivity:activity_];
    [window_ close];
}

bool MacPlatform::create(const Config& cfg, std::string& err) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    // Keep App Nap and timer coalescing away from the frame pacing.
    activity_ = [[NSProcessInfo processInfo]
        beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                          reason:@"VRR timing test"];

    // Open on the screen under the mouse, where the user started the app.
    NSScreen* screen = NSScreen.mainScreen;
    const NSPoint mouse = NSEvent.mouseLocation;
    for (NSScreen* s in NSScreen.screens)
        if (NSPointInRect(mouse, s.frame)) screen = s;
    const NSRect vis = screen.visibleFrame;
    const CGFloat w = std::min<CGFloat>(cfg.windowW, vis.size.width), h = std::min<CGFloat>(cfg.windowH, vis.size.height);
    const NSRect rect = NSMakeRect(vis.origin.x + (vis.size.width - w) / 2, vis.origin.y + (vis.size.height - h) / 2, w, h);
    window_ = [[NSWindow alloc]
        initWithContentRect:rect
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (!window_) return err = "cannot create window", false;
    window_.title = @"Synchora - VRR flicker check";
    window_.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    window_.releasedWhenClosed = NO;
    view_ = [[VrrView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
    view_.wantsLayer = YES;
    view_.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    view_.keys = &g_keys;
    window_.contentView = view_;
    g_delegate = [VrrWindowDelegate new];
    g_delegate.view = view_;
    window_.delegate = g_delegate;
    [window_ makeKeyAndOrderFront:nil];
    [window_ makeFirstResponder:view_];
    [NSApp activateIgnoringOtherApps:YES];
    [view_ updateDrawableSize];
    if (![view_.layer isKindOfClass:[CAMetalLayer class]]) return err = "view is not backed by a CAMetalLayer", false;
    return true;
}

CAMetalLayer* MacPlatform::layer() const { return (CAMetalLayer*)view_.layer; }

void MacPlatform::pollEvents(std::vector<KeyEvent>& keys) {
    @autoreleasepool {
        for (;;) {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (!e) break;
            [NSApp sendEvent:e];
        }
    }
    keys.insert(keys.end(), g_keys.begin(), g_keys.end());
    g_keys.clear();
}

bool MacPlatform::quitRequested() const { return view_.quit; }

WindowMode MacPlatform::windowMode() const {
    return (window_.styleMask & NSWindowStyleMaskFullScreen) ? WindowMode::Borderless : WindowMode::Windowed;
}

void MacPlatform::setWindowMode(WindowMode m) {
    const bool fs = (window_.styleMask & NSWindowStyleMaskFullScreen) != 0;
    const bool want = m != WindowMode::Windowed;
    if (fs != want) [window_ toggleFullScreen:nil];
    if (want) [NSCursor setHiddenUntilMouseMoves:YES];
}

void MacPlatform::screenVrrRange(double& minHz, double& maxHz) const {
    minHz = maxHz = 0;
    if (@available(macOS 12.0, *)) {
        NSScreen* s = screenOf(window_);
        const double minInterval = s.minimumRefreshInterval, maxInterval = s.maximumRefreshInterval;
        if (minInterval > 0 && maxInterval > 0) {
            const double hi = 1.0 / minInterval, lo = 1.0 / maxInterval;
            if (hi - lo > 5.0) {
                minHz = std::round(lo);
                maxHz = std::round(hi);
            }
        }
    }
}

DisplayInfo MacPlatform::displayInfo() {
    DisplayInfo d;
    NSScreen* s = screenOf(window_);
    const CGDirectDisplayID id = displayIdOf(s);
    d.outputName = CGDisplayIsBuiltin(id) ? "Built-in display" : "Display " + std::to_string(id);
    if (@available(macOS 10.15, *)) d.monitorName = s.localizedName.UTF8String ? s.localizedName.UTF8String : "";
    if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
        d.width = static_cast<int>(CGDisplayModeGetPixelWidth(mode));
        d.height = static_cast<int>(CGDisplayModeGetPixelHeight(mode));
        d.refreshHz = CGDisplayModeGetRefreshRate(mode);
        CGDisplayModeRelease(mode);
    }
    if (d.refreshHz <= 0) {
        if (@available(macOS 12.0, *)) d.refreshHz = static_cast<double>(s.maximumFramesPerSecond);
    }
    screenVrrRange(d.osVrrMinHz, d.osVrrMaxHz);
    if (d.osVrrMaxHz > 0) {
        d.osRangeSource = "reported by macOS";
        d.refreshHz = std::max(d.refreshHz, d.osVrrMaxHz);
    }
    char key[64];
    std::snprintf(key, sizeof key, "mac-%08X-%08X-%08X", CGDisplayVendorNumber(id), CGDisplayModelNumber(id),
                  CGDisplaySerialNumber(id));
    d.monitorKey = key;
    d.extra = d.osVrrMaxHz > 0 ? "macOS reports variable refresh for this display"
                               : "macOS reports a fixed refresh rate for this display";
    return d;
}

}  // namespace vrr
