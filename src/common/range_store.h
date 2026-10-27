// Remembers the VRR range the user entered for each monitor, so it is asked only once.
#pragma once

#include <string>

namespace vrr {

struct SavedRange {
    double minHz = 0, maxHz = 0;
    double lfcFps = 0;       // 0 = automatic
    bool useManual = true;   // false: the user switched back to the detected range but kept this entry
};

// Per-user settings directory: $XDG_CONFIG_HOME/synchora, %APPDATA%\Synchora or
// ~/Library/Application Support/Synchora. Empty if it cannot be determined.
std::string configDir();

bool loadRange(const std::string& monitorKey, SavedRange& out);
// Returns the file written to, or an empty string on failure.
std::string saveRange(const std::string& monitorKey, const SavedRange& r);

}  // namespace vrr
