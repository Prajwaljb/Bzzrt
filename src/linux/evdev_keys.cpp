#include <linux/input-event-codes.h>

#include "vk_window.h"

namespace vrr {

Key keyFromEvdev(uint32_t code) {
    switch (code) {
        case KEY_ESC: return Key::Escape;
        case KEY_Q: return Key::Q;
        case KEY_SPACE: return Key::Space;
        case KEY_UP: return Key::Up;
        case KEY_DOWN: return Key::Down;
        case KEY_LEFT: return Key::Left;
        case KEY_RIGHT: return Key::Right;
        case KEY_PAGEUP: return Key::PageUp;
        case KEY_PAGEDOWN: return Key::PageDown;
        case KEY_EQUAL:
        case KEY_KPPLUS: return Key::Plus;
        case KEY_MINUS:
        case KEY_KPMINUS: return Key::Minus;
        case KEY_F11:
        case KEY_F: return Key::Fullscreen;
        case KEY_V: return Key::V;
        case KEY_P: return Key::P;
        case KEY_G: return Key::G;
        case KEY_C: return Key::C;
        case KEY_H: return Key::H;
        case KEY_L: return Key::L;
        case KEY_R: return Key::R;
        case KEY_A: return Key::A;
        case KEY_K: return Key::K;
        case KEY_LEFTBRACE: return Key::LBracket;
        case KEY_RIGHTBRACE: return Key::RBracket;
        case KEY_Y: return Key::Y;
        case KEY_N: return Key::N;
        case KEY_S: return Key::S;
        case KEY_ENTER:
        case KEY_KPENTER: return Key::Enter;
        case KEY_TAB: return Key::Tab;
        case KEY_BACKSPACE: return Key::Backspace;
        case KEY_0: case KEY_KP0: return Key::D0;
        case KEY_1: case KEY_KP1: return Key::D1;
        case KEY_2: case KEY_KP2: return Key::D2;
        case KEY_3: case KEY_KP3: return Key::D3;
        case KEY_4: case KEY_KP4: return Key::D4;
        case KEY_5: case KEY_KP5: return Key::D5;
        case KEY_6: case KEY_KP6: return Key::D6;
        case KEY_7: case KEY_KP7: return Key::D7;
        case KEY_8: case KEY_KP8: return Key::D8;
        case KEY_9: case KEY_KP9: return Key::D9;
        default: return Key::None;
    }
}

}  // namespace vrr
