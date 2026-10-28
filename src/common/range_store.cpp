#include "range_store.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace vrr {

namespace fs = std::filesystem;

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// The settings directory under a given app name ("Bzzrt" on Windows and macOS, "bzzrt" on Linux).
std::string configDirNamed(const char* name) {
#if defined(_WIN32)
    const std::string base = env("APPDATA");
    return base.empty() ? std::string() : (fs::path(base) / name).string();
#elif defined(__APPLE__)
    const std::string home = env("HOME");
    return home.empty() ? std::string() : (fs::path(home) / "Library" / "Application Support" / name).string();
#else
    std::string base = env("XDG_CONFIG_HOME");
    if (base.empty()) {
        const std::string home = env("HOME");
        if (home.empty()) return {};
        base = (fs::path(home) / ".config").string();
    }
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return (fs::path(base) / lower).string();
#endif
}

std::string storePath() {
    const std::string dir = configDir();
    return dir.empty() ? std::string() : (fs::path(dir) / "ranges.txt").string();
}

// Ranges saved before the app was renamed from Synchora; read until the first save writes the new file.
std::string legacyStorePath() {
    const std::string dir = configDirNamed("Synchora");
    return dir.empty() ? std::string() : (fs::path(dir) / "ranges.txt").string();
}

// One monitor per line: key, minimum Hz, maximum Hz, LFC threshold FPS (0 = automatic), "manual" or "auto"
// (which range is in use; older files have no fifth column and mean "manual").
std::vector<std::pair<std::string, SavedRange>> readAll() {
    std::vector<std::pair<std::string, SavedRange>> out;
    std::error_code ec;
    const std::string path = storePath();
    std::ifstream f(!path.empty() && !fs::exists(path, ec) ? legacyStorePath() : path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::string key;
        SavedRange r;
        if (is >> key >> r.minHz >> r.maxHz) {
            std::string use;
            if (!(is >> r.lfcFps)) r.lfcFps = 0;
            else if (is >> use) r.useManual = use != "auto";
            if (r.minHz > 0 && r.maxHz > r.minHz) out.push_back({key, r});
        }
    }
    return out;
}

}  // namespace

std::string configDir() { return configDirNamed("Bzzrt"); }

bool loadRange(const std::string& monitorKey, SavedRange& out) {
    if (monitorKey.empty()) return false;
    for (const auto& [key, r] : readAll())
        if (key == monitorKey) {
            out = r;
            return true;
        }
    return false;
}

std::string saveRange(const std::string& monitorKey, const SavedRange& r) {
    const std::string path = storePath();
    if (monitorKey.empty() || path.empty()) return {};
    auto all = readAll();
    bool found = false;
    for (auto& [key, v] : all)
        if (key == monitorKey) v = r, found = true;
    if (!found) all.push_back({monitorKey, r});
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return {};
    f << "# Bzzrt: VRR range entered per monitor (key min_hz max_hz lfc_threshold_fps use)\n"
         "# lfc_threshold_fps 0 = automatic; use = manual (this entry) or auto (the detected range)\n";
    for (const auto& [key, v] : all)
        f << key << ' ' << v.minHz << ' ' << v.maxHz << ' ' << v.lfcFps << ' ' << (v.useManual ? "manual" : "auto")
          << '\n';
    return f ? path : std::string();
}

}  // namespace vrr
