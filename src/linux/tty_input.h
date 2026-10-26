// Keyboard input from the controlling terminal (used by the DRM/KMS backend,
// which runs on a text console without a window system).
#pragma once

#include <termios.h>

#include <vector>

#include "../common/backend.h"

namespace vrr {

class TtyInput {
public:
    ~TtyInput();
    bool init();  // false if stdin is not a terminal (input then unavailable)
    void poll(std::vector<KeyEvent>& keys);
    bool quitRequested() const;

private:
    bool active_ = false;
    termios saved_{};
};

}  // namespace vrr
