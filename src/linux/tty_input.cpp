#include "tty_input.h"

#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstring>

namespace vrr {

namespace {
volatile std::sig_atomic_t g_quit = 0;
void onSignal(int) { g_quit = 1; }
}  // namespace

TtyInput::~TtyInput() {
    if (active_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
}

bool TtyInput::init() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGHUP, onSignal);
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_) != 0) return false;
    termios raw = saved_;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);  // keep ISIG so Ctrl+C still works
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
    active_ = true;
    return true;
}

bool TtyInput::quitRequested() const { return g_quit != 0; }

void TtyInput::poll(std::vector<KeyEvent>& keys) {
    if (!active_) return;
    unsigned char buf[64];
    const ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n <= 0) return;
    for (ssize_t i = 0; i < n; ++i) {
        const unsigned char c = buf[i];
        if (c == 0x1b) {
            if (i + 1 >= n || buf[i + 1] != '[') {
                keys.push_back({Key::Escape, false});
                continue;
            }
            // CSI sequence: ESC [ params final
            ssize_t j = i + 2;
            int param = 0;
            while (j < n && buf[j] >= '0' && buf[j] <= '9') param = param * 10 + (buf[j++] - '0');
            if (j >= n) break;
            const unsigned char fin = buf[j];
            Key k = Key::None;
            if (fin == 'A') k = Key::Up;
            else if (fin == 'B') k = Key::Down;
            else if (fin == 'C') k = Key::Right;
            else if (fin == 'D') k = Key::Left;
            else if (fin == '~' && param == 5) k = Key::PageUp;
            else if (fin == '~' && param == 6) k = Key::PageDown;
            else if (fin == '~' && param == 23) k = Key::Fullscreen;
            if (k != Key::None) keys.push_back({k, false});
            i = j;
            continue;
        }
        Key k = Key::None;
        bool shift = false;
        switch (c) {
            case ' ': k = Key::Space; break;
            case '+': k = Key::Plus; break;
            case '=': k = Key::Plus; break;
            case '-': k = Key::Minus; break;
            case '_': k = Key::Minus; shift = true; break;
            case '[': k = Key::LBracket; break;
            case ']': k = Key::RBracket; break;
            case '\r':
            case '\n': k = Key::Enter; break;
            case '\t': k = Key::Tab; break;
            default: {
                const unsigned char lower = (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c;
                shift = c >= 'A' && c <= 'Z';
                switch (lower) {
                    case 'q': k = Key::Q; break;
                    case 'f': k = Key::Fullscreen; break;
                    case 'v': k = Key::V; break;
                    case 'p': k = Key::P; break;
                    case 'g': k = Key::G; break;
                    case 'c': k = Key::C; break;
                    case 'h': k = Key::H; break;
                    case 'l': k = Key::L; break;
                    case 'r': k = Key::R; break;
                    case 'k': k = Key::K; break;
                    case 'y': k = Key::Y; break;
                    case 'n': k = Key::N; break;
                    case 's': k = Key::S; break;
                    default: break;
                }
            }
        }
        if (k != Key::None) keys.push_back({k, shift});
    }
}

}  // namespace vrr
