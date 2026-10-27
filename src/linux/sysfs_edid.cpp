#include "sysfs_edid.h"

#include <dirent.h>

#include <fstream>
#include <iterator>

namespace vrr {

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool connected(const std::string& dir) {
    std::ifstream f(dir + "/status");
    std::string s;
    return std::getline(f, s) && s == "connected";
}

}  // namespace

std::vector<uint8_t> readSysfsEdid(const std::string& connector) {
    const std::string root = "/sys/class/drm/";
    DIR* d = opendir(root.c_str());
    if (!d) return {};
    std::vector<uint8_t> exact, onlyExternal;
    int externals = 0;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        // Entries look like "card1-DP-2"; render nodes and bare cards have no '-'.
        const size_t dash = name.find('-');
        if (name.compare(0, 4, "card") != 0 || dash == std::string::npos) continue;
        const std::string conn = name.substr(dash + 1);
        const std::string dir = root + name;
        if (!connected(dir)) continue;
        std::vector<uint8_t> edid = readFile(dir + "/edid");
        if (edid.size() < 128) continue;
        if (!connector.empty() && conn == connector) exact = edid;
        if (conn.compare(0, 3, "eDP") != 0 && conn.compare(0, 4, "LVDS") != 0 && conn.compare(0, 3, "DSI") != 0) {
            ++externals;
            onlyExternal = edid;
        }
    }
    closedir(d);
    if (!exact.empty()) return exact;
    return externals == 1 && connector.empty() ? onlyExternal : std::vector<uint8_t>{};
}

}  // namespace vrr
