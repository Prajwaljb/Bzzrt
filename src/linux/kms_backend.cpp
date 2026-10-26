#include "kms_backend.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/clock.h"
#include "../common/overlay.h"
#include "../common/patterns.h"

#ifndef DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15
#endif

namespace vrr {

namespace {

thread_local KmsBackend* g_activeKms = nullptr;

void pageFlipHandler(int, unsigned int, unsigned int sec, unsigned int usec, unsigned int, void* user) {
    if (g_activeKms)
        g_activeKms->onFlip(reinterpret_cast<uintptr_t>(user),
                            static_cast<int64_t>(sec) * 1000000000LL + static_cast<int64_t>(usec) * 1000LL);
}

const char* connectorTypeName(uint32_t t) {
    switch (t) {
        case DRM_MODE_CONNECTOR_VGA: return "VGA";
        case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
        case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
        case DRM_MODE_CONNECTOR_eDP: return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
        case DRM_MODE_CONNECTOR_DSI: return "DSI";
        case DRM_MODE_CONNECTOR_DPI: return "DPI";
        default: return "Unknown";
    }
}

double modeRefresh(const drmModeModeInfo& m) {
    double vtotal = m.vtotal;
    if (m.flags & DRM_MODE_FLAG_DBLSCAN) vtotal *= 2;
    if (m.flags & DRM_MODE_FLAG_INTERLACE) vtotal /= 2;
    if (!m.htotal || vtotal <= 0) return 0.0;
    return m.clock * 1000.0 / (m.htotal * vtotal);
}

const char* errnoName(int e) {
    switch (e) {
        case 0: return "OK";
        case EBUSY: return "EBUSY";
        case EINVAL: return "EINVAL";
        case EACCES: return "EACCES (lost DRM master?)";
        case EPERM: return "EPERM";
        case ENOMEM: return "ENOMEM";
        case ENOSPC: return "ENOSPC";
        default: return "ERROR";
    }
}

}  // namespace

KmsBackend::~KmsBackend() {
    if (fd_ >= 0) {
        restore();
        destroyBuffers();
        if (modeBlob_) drmModeDestroyPropertyBlob(fd_, modeBlob_);
        drmDropMaster(fd_);
        close(fd_);
    }
    if (savedCrtc_) drmModeFreeCrtc(savedCrtc_);
}

uint32_t KmsBackend::propId(uint32_t obj, uint32_t type, const char* name, uint64_t* value) const {
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd_, obj, type);
    if (!props) return 0;
    uint32_t id = 0;
    for (uint32_t i = 0; i < props->count_props && !id; ++i) {
        drmModePropertyRes* p = drmModeGetProperty(fd_, props->props[i]);
        if (!p) continue;
        if (!std::strcmp(p->name, name)) {
            id = p->prop_id;
            if (value) *value = props->prop_values[i];
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

bool KmsBackend::init(const Config& cfg, const ImageData& image, std::string& err) {
    image_ = &image;
    want10_ = cfg.tenBit;
    if (!tty_.init())
        std::fprintf(stderr, "stdin is not a terminal: keyboard control disabled (Ctrl+C quits)\n");

    std::vector<std::string> cards;
    if (!cfg.kmsDevice.empty()) {
        cards.push_back(cfg.kmsDevice);
    } else {
        for (int i = 0; i < 16; ++i) {
            const std::string p = "/dev/dri/card" + std::to_string(i);
            if (access(p.c_str(), F_OK) == 0) cards.push_back(p);
        }
    }
    // Pass 0: prefer connectors that advertise vrr_capable; pass 1: any connected display.
    for (int pass = 0; pass < 2 && fd_ < 0; ++pass) {
        for (const auto& c : cards) {
            vrrCapable_ = false;
            if (tryCard(c, cfg, err) && (pass == 1 || vrrCapable_ || !cfg.kmsConnector.empty())) break;
            if (fd_ >= 0) {
                close(fd_);
                fd_ = -1;
            }
        }
    }
    if (fd_ < 0) {
        if (err.empty()) err = "no connected display found on any /dev/dri/card*";
        return false;
    }
    return setup(cfg, err);
}

bool KmsBackend::tryCard(const std::string& path, const Config& cfg, std::string& err) {
    fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        err = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    drmSetClientCap(fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModeRes* res = drmModeGetResources(fd_);
    if (!res) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    bool found = false;
    for (int i = 0; i < res->count_connectors && !found; ++i) {
        drmModeConnector* c = drmModeGetConnector(fd_, res->connectors[i]);
        if (!c) continue;
        const std::string name = std::string(connectorTypeName(c->connector_type)) + "-" +
                                 std::to_string(c->connector_type_id);
        const bool usable = c->connection == DRM_MODE_CONNECTED && c->count_modes > 0;
        const bool nameOk = cfg.kmsConnector.empty() || cfg.kmsConnector == name;
        if (usable && nameOk) {
            uint64_t cap = 0;
            propId(c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "vrr_capable", &cap);
            // Skip the built-in panel unless explicitly requested or nothing else exists.
            const bool internal = c->connector_type == DRM_MODE_CONNECTOR_eDP ||
                                  c->connector_type == DRM_MODE_CONNECTOR_LVDS ||
                                  c->connector_type == DRM_MODE_CONNECTOR_DSI;
            if (!internal || !cfg.kmsConnector.empty() || cap) {
                found = true;
                connId_ = c->connector_id;
                connName_ = name;
                vrrCapable_ = cap != 0;
                cardPath_ = path;
            }
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    if (!found) {
        close(fd_);
        fd_ = -1;
    }
    return found;
}

bool KmsBackend::setup(const Config& cfg, std::string& err) {
    if (drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
        return err = "driver does not support atomic modesetting on " + cardPath_, false;
    if (!drmIsMaster(fd_) && drmSetMaster(fd_) != 0) {
        err = "cannot become DRM master on " + cardPath_ +
              ": a compositor/X server owns the display. Switch to a free text console "
              "(Ctrl+Alt+F3), log in there and run again.";
        return false;
    }
    if (drmVersionPtr ver = drmGetVersion(fd_)) {
        driverName_ = std::string(ver->name ? ver->name : "?") + " (" + (ver->desc ? ver->desc : "") + ")";
        drmFreeVersion(ver);
    }
    uint64_t cap = 0;
    asyncSupported_ = drmGetCap(fd_, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &cap) == 0 && cap;

    drmModeRes* res = drmModeGetResources(fd_);
    drmModeConnector* conn = drmModeGetConnector(fd_, connId_);
    if (!res || !conn) return err = "cannot query DRM resources", false;

    // CRTC: prefer the one currently driving the connector.
    crtcId_ = 0;
    if (conn->encoder_id) {
        if (drmModeEncoder* enc = drmModeGetEncoder(fd_, conn->encoder_id)) {
            crtcId_ = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    }
    for (int e = 0; e < conn->count_encoders && !crtcId_; ++e) {
        drmModeEncoder* enc = drmModeGetEncoder(fd_, conn->encoders[e]);
        if (!enc) continue;
        for (int i = 0; i < res->count_crtcs; ++i)
            if (enc->possible_crtcs & (1u << i)) {
                crtcId_ = res->crtcs[i];
                break;
            }
        drmModeFreeEncoder(enc);
    }
    for (int i = 0; i < res->count_crtcs; ++i)
        if (res->crtcs[i] == crtcId_) crtcIndex_ = i;
    if (!crtcId_) return err = "no CRTC available for " + connName_, false;

    // Mode: requested WxH@Hz, else the preferred resolution at its highest refresh rate.
    int reqW = 0, reqH = 0;
    double reqHz = 0;
    if (!cfg.kmsMode.empty()) std::sscanf(cfg.kmsMode.c_str(), "%dx%d@%lf", &reqW, &reqH, &reqHz);
    if (reqW <= 0) {
        int pref = 0;
        for (int i = 0; i < conn->count_modes; ++i)
            if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
                pref = i;
                break;
            }
        reqW = conn->modes[pref].hdisplay;
        reqH = conn->modes[pref].vdisplay;
    }
    int best = -1;
    for (int i = 0; i < conn->count_modes; ++i) {
        const auto& m = conn->modes[i];
        if (m.hdisplay != reqW || m.vdisplay != reqH || (m.flags & DRM_MODE_FLAG_INTERLACE)) continue;
        if (best < 0) {
            best = i;
            continue;
        }
        const double r = modeRefresh(m), rb = modeRefresh(conn->modes[best]);
        if (reqHz > 0 ? std::fabs(r - reqHz) < std::fabs(rb - reqHz) : r > rb) best = i;
    }
    if (best < 0) return err = "requested mode not offered by " + connName_, false;
    mode_ = conn->modes[best];
    width_ = mode_.hdisplay;
    height_ = mode_.vdisplay;

    // EDID: monitor name and range limits.
    uint64_t edidBlob = 0;
    if (propId(connId_, DRM_MODE_OBJECT_CONNECTOR, "EDID", &edidBlob) && edidBlob) {
        if (drmModePropertyBlobRes* b = drmModeGetPropertyBlob(fd_, static_cast<uint32_t>(edidBlob))) {
            const auto* d = static_cast<const uint8_t*>(b->data);
            if (b->length >= 128) {
                for (int off : {54, 72, 90, 108}) {
                    const uint8_t* q = d + off;
                    if (q[0] || q[1] || q[2]) continue;
                    if (q[3] == 0xFC) {
                        monitorName_.clear();
                        for (int k = 5; k < 18 && q[k] != 0x0A; ++k) monitorName_ += static_cast<char>(q[k]);
                    } else if (q[3] == 0xFD) {
                        const int minV = q[5] + ((q[4] & 3) == 3 ? 255 : 0);
                        const int maxV = q[6] + ((q[4] & 2) ? 255 : 0);
                        edidRange_ = "EDID range limits: " + std::to_string(minV) + "-" + std::to_string(maxV) + " Hz";
                    }
                }
            }
            drmModeFreePropertyBlob(b);
        }
    }
    propId(connId_, DRM_MODE_OBJECT_CONNECTOR, "max bpc", &maxBpc_);

    // Primary plane for the CRTC.
    drmModePlaneRes* planes = drmModeGetPlaneResources(fd_);
    for (uint32_t i = 0; planes && i < planes->count_planes && !planeId_; ++i) {
        drmModePlane* pl = drmModeGetPlane(fd_, planes->planes[i]);
        if (!pl) continue;
        uint64_t type = 0;
        propId(pl->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
        if ((pl->possible_crtcs & (1u << crtcIndex_)) && type == DRM_PLANE_TYPE_PRIMARY) {
            bool has8 = false;
            for (uint32_t f = 0; f < pl->count_formats; ++f) {
                if (pl->formats[f] == DRM_FORMAT_XRGB8888) has8 = true;
                if (pl->formats[f] == DRM_FORMAT_XRGB2101010) plane10_ = true;
            }
            if (has8) planeId_ = pl->plane_id;
        }
        drmModeFreePlane(pl);
    }
    if (planes) drmModeFreePlaneResources(planes);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!planeId_) return err = "no primary plane with XRGB8888 for the CRTC", false;
    want10_ = want10_ && plane10_;

    pConnCrtc_ = propId(connId_, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    pCrtcMode_ = propId(crtcId_, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    pCrtcActive_ = propId(crtcId_, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    pCrtcVrr_ = propId(crtcId_, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", &savedVrr_);
    pFb_ = propId(planeId_, DRM_MODE_OBJECT_PLANE, "FB_ID");
    pPlaneCrtc_ = propId(planeId_, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    pSrcX_ = propId(planeId_, DRM_MODE_OBJECT_PLANE, "SRC_X");
    pSrcY_ = propId(planeId_, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    pSrcW_ = propId(planeId_, DRM_MODE_OBJECT_PLANE, "SRC_W");
    pSrcH_ = propId(planeId_, DRM_MODE_OBJECT_PLANE, "SRC_H");
    pCrtcX_ = propId(planeId_, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    pCrtcY_ = propId(planeId_, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    pCrtcW_ = propId(planeId_, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    pCrtcH_ = propId(planeId_, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    if (!pConnCrtc_ || !pCrtcMode_ || !pCrtcActive_ || !pFb_ || !pPlaneCrtc_)
        return err = "missing required KMS properties", false;

    savedCrtc_ = drmModeGetCrtc(fd_, crtcId_);
    if (drmModeCreatePropertyBlob(fd_, &mode_, sizeof mode_, &modeBlob_) != 0)
        return err = "cannot create mode blob", false;

    if (!createBuffers(want10_, err)) return false;
    vrrActive_ = pCrtcVrr_ != 0;
    modeIdx_ = kModeVrr;
    if (!commitModeset(0, vrrActive_, err)) return false;

    std::printf("KMS: %s %s on %s, %dx%d@%.3f Hz, VRR_ENABLED=%d, vrr_capable=%d\n", driverName_.c_str(),
                connName_.c_str(), cardPath_.c_str(), width_, height_, modeRefresh(mode_), vrrActive_ ? 1 : 0,
                vrrCapable_ ? 1 : 0);
    return true;
}

bool KmsBackend::createBuffers(bool tenBit, std::string& err) {
    const uint32_t fmt = tenBit ? DRM_FORMAT_XRGB2101010 : DRM_FORMAT_XRGB8888;
    for (auto& b : bufs_) {
        b = Buffer{};
        drm_mode_create_dumb creq{};
        creq.width = static_cast<uint32_t>(width_);
        creq.height = static_cast<uint32_t>(height_);
        creq.bpp = 32;
        if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) return err = "DRM_IOCTL_MODE_CREATE_DUMB failed", false;
        b.handle = creq.handle;
        b.pitch = creq.pitch;
        b.size = creq.size;
        const uint32_t handles[4] = {b.handle, 0, 0, 0};
        const uint32_t pitches[4] = {b.pitch, 0, 0, 0};
        const uint32_t offsets[4] = {0, 0, 0, 0};
        if (drmModeAddFB2(fd_, creq.width, creq.height, fmt, handles, pitches, offsets, &b.fbId, 0) != 0)
            return err = "drmModeAddFB2 failed", false;
        drm_mode_map_dumb mreq{};
        mreq.handle = b.handle;
        if (drmIoctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) return err = "DRM_IOCTL_MODE_MAP_DUMB failed", false;
        void* p = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, static_cast<off_t>(mreq.offset));
        if (p == MAP_FAILED) return err = "mmap of dumb buffer failed", false;
        b.map = static_cast<uint8_t*>(p);
        std::memset(b.map, 0, b.size);
    }
    is10_ = tenBit;
    basePattern_ = -1;
    displayed_ = 0;
    pending_ = -1;
    return true;
}

void KmsBackend::destroyBuffers() {
    for (auto& b : bufs_) {
        if (b.map) munmap(b.map, b.size);
        if (b.fbId) drmModeRmFB(fd_, b.fbId);
        if (b.handle) {
            drm_mode_destroy_dumb dreq{};
            dreq.handle = b.handle;
            drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        }
        b = Buffer{};
    }
}

bool KmsBackend::commitModeset(int buffer, bool vrr, std::string& err) {
    drmModeAtomicReq* req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, connId_, pConnCrtc_, crtcId_);
    drmModeAtomicAddProperty(req, crtcId_, pCrtcMode_, modeBlob_);
    drmModeAtomicAddProperty(req, crtcId_, pCrtcActive_, 1);
    if (pCrtcVrr_) drmModeAtomicAddProperty(req, crtcId_, pCrtcVrr_, vrr ? 1 : 0);
    drmModeAtomicAddProperty(req, planeId_, pFb_, bufs_[buffer].fbId);
    drmModeAtomicAddProperty(req, planeId_, pPlaneCrtc_, crtcId_);
    drmModeAtomicAddProperty(req, planeId_, pSrcX_, 0);
    drmModeAtomicAddProperty(req, planeId_, pSrcY_, 0);
    drmModeAtomicAddProperty(req, planeId_, pSrcW_, static_cast<uint64_t>(width_) << 16);
    drmModeAtomicAddProperty(req, planeId_, pSrcH_, static_cast<uint64_t>(height_) << 16);
    drmModeAtomicAddProperty(req, planeId_, pCrtcX_, 0);
    drmModeAtomicAddProperty(req, planeId_, pCrtcY_, 0);
    drmModeAtomicAddProperty(req, planeId_, pCrtcW_, static_cast<uint64_t>(width_));
    drmModeAtomicAddProperty(req, planeId_, pCrtcH_, static_cast<uint64_t>(height_));
    const int r = drmModeAtomicCommit(fd_, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
    drmModeAtomicFree(req);
    if (r != 0) {
        err = std::string("atomic modeset failed: ") + std::strerror(-r);
        return false;
    }
    displayed_ = buffer;
    pending_ = -1;
    return true;
}

void KmsBackend::onFlip(uint64_t frame, int64_t ns) {
    if (pending_ >= 0) displayed_ = pending_;
    pending_ = -1;
    flips_.push_back({frame, ns});
    lastFlipNs_ = nowNs();
}

void KmsBackend::handleEvents(int timeoutMs) {
    pollfd pfd{fd_, POLLIN, 0};
    if (poll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & POLLIN)) {
        drmEventContext ctx{};
        ctx.version = 3;
        ctx.page_flip_handler2 = pageFlipHandler;
        g_activeKms = this;
        drmHandleEvent(fd_, &ctx);
        g_activeKms = nullptr;
    }
}

void KmsBackend::waitForFlip() {
    const int64_t deadline = nowNs() + 1000000000LL;
    while (pending_ >= 0) {
        if (nowNs() > deadline) {
            lastError_ = "page flip event timed out";
            pending_ = -1;
            break;
        }
        handleEvents(100);
    }
}

void KmsBackend::rebuildBase() {
    base_.resize(static_cast<size_t>(width_) * height_);
    const float s = is10_ ? 1023.0f : 255.0f;
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const Rgb c = staticPatternColor(basePattern_, x, y, width_, height_, image_);
            const uint32_t r = static_cast<uint32_t>(std::clamp(c.r, 0.0f, 1.0f) * s + 0.5f);
            const uint32_t g = static_cast<uint32_t>(std::clamp(c.g, 0.0f, 1.0f) * s + 0.5f);
            const uint32_t b = static_cast<uint32_t>(std::clamp(c.b, 0.0f, 1.0f) * s + 0.5f);
            base_[static_cast<size_t>(y) * width_ + x] = is10_ ? (r << 20) | (g << 10) | b : (r << 16) | (g << 8) | b;
        }
    }
}

void KmsBackend::compose(uint8_t* dstBase, size_t pitch, int originX, int originY, Rect r, const FrameParams& p) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(width_, r.x + r.w), y1 = std::min(height_, r.y + r.h);
    if (x0 >= x1 || y0 >= y1) return;
    const int shift = is10_ ? 10 : 8;
    const uint32_t maxv = is10_ ? 1023u : 255u;
    auto pack = [&](uint32_t r8, uint32_t g8, uint32_t b8) {
        return ((r8 * maxv / 255) << (2 * shift)) | ((g8 * maxv / 255) << shift) | (b8 * maxv / 255);
    };
    const uint32_t white = pack(255, 255, 255), black = 0, dim = pack(38, 38, 38);

    const bool moving = p.pattern == kPatMoving;
    double wx = 0, bx = 0;
    if (moving) moveBars(moveBarX(p.patternTime, width_), width_, wx, bx);
    int cx, cy, cw, ch;
    counterRect(width_, cx, cy, cw, ch);
    const bool ov = p.overlayVisible && p.overlayPixels;
    const int ox = p.overlayX, oy = p.overlayY, ow = p.overlayW, oh = p.overlayH;

    for (int y = y0; y < y1; ++y) {
        auto* dst = reinterpret_cast<uint32_t*>(dstBase + static_cast<size_t>(y - originY) * pitch) - originX;
        const uint32_t* src = base_.data() + static_cast<size_t>(y) * width_;
        const bool ovRow = ov && y >= oy && y < oy + oh;
        const bool ctrRow = p.frameCounter && y >= cy && y < cy + ch;
        for (int x = x0; x < x1; ++x) {
            uint32_t v = src[x];
            if (moving) {
                const double fx = x + 0.5;
                if (fx >= wx && fx < wx + kMoveBarWidth) v = white;
                else if (fx >= bx && fx < bx + kMoveBarWidth) v = black;
            }
            if (ctrRow && x >= cx && x < cx + cw) {
                v = black;
                const double lx = x + 0.5 - cx - kCounterGap, ly = y + 0.5 - cy - kCounterGap;
                if (lx >= 0 && ly >= 0 && ly < kCounterCell) {
                    const int i = static_cast<int>(lx / (kCounterCell + kCounterGap));
                    const double inCell = lx - i * (kCounterCell + kCounterGap);
                    if (i < kCounterBits && inCell < kCounterCell)
                        v = ((p.frameIndex >> (kCounterBits - 1 - i)) & 1) ? white : dim;
                }
            }
            if (ovRow && x >= ox && x < ox + ow) {
                const uint32_t o = p.overlayPixels[static_cast<size_t>(y - oy) * kOverlayW + (x - ox)];
                const uint32_t a = o >> 24;
                if (a) {
                    const uint32_t mask = maxv;
                    const uint32_t br = (v >> (2 * shift)) & mask, bg = (v >> shift) & mask, bb = v & mask;
                    const uint32_t orr = (o & 255) * maxv / 255, og = ((o >> 8) & 255) * maxv / 255,
                                   ob = ((o >> 16) & 255) * maxv / 255;
                    const uint32_t nr = (orr * a + br * (255 - a)) / 255;
                    const uint32_t ng = (og * a + bg * (255 - a)) / 255;
                    const uint32_t nb = (ob * a + bb * (255 - a)) / 255;
                    v = (nr << (2 * shift)) | (ng << shift) | nb;
                }
            }
            dst[x] = v;
        }
    }
}

bool KmsBackend::blitOverlay(Buffer& b, Rect r, const FrameParams& p) {
    // Only the static patterns are cacheable, and the frame counter must not overlap the overlay.
    if (p.pattern == kPatMoving || !p.overlayVisible || r.w <= 0 || r.h <= 0) return false;
    if (r.x < 0 || r.y < 0 || r.x + r.w > width_ || r.y + r.h > height_) return false;
    if (p.frameCounter) {
        int cx, cy, cw, ch;
        counterRect(width_, cx, cy, cw, ch);
        if (cx < r.x + r.w && r.x < cx + cw && cy < r.y + r.h && r.y < cy + ch) return false;
    }
    const bool same = ovCacheVersion_ == p.overlayVersion && ovCachePattern_ == p.pattern && ovCache10_ == is10_ &&
                      ovCacheRect_.x == r.x && ovCacheRect_.y == r.y && ovCacheRect_.w == r.w && ovCacheRect_.h == r.h;
    if (!same) {
        ovCache_.resize(static_cast<size_t>(r.w) * r.h);
        compose(reinterpret_cast<uint8_t*>(ovCache_.data()), static_cast<size_t>(r.w) * 4, r.x, r.y, r, p);
        ovCacheRect_ = r;
        ovCacheVersion_ = p.overlayVersion;
        ovCachePattern_ = p.pattern;
        ovCache10_ = is10_;
    }
    for (int y = 0; y < r.h; ++y)
        std::memcpy(b.map + static_cast<size_t>(r.y + y) * b.pitch + static_cast<size_t>(r.x) * 4,
                    ovCache_.data() + static_cast<size_t>(y) * r.w, static_cast<size_t>(r.w) * 4);
    return true;
}

RenderResult KmsBackend::renderFrame(const FrameParams& p) {
    if (fd_ < 0) return RenderResult::Fatal;

    if (want10_ != is10_) {
        waitForFlip();
        std::string err;
        destroyBuffers();
        if (!createBuffers(want10_, err) || !commitModeset(0, vrrActive_, err)) {
            std::fprintf(stderr, "KMS: format change failed: %s\n", err.c_str());
            return RenderResult::Fatal;
        }
    }
    const bool wantVrr = modeIdx_ != kModeFixed;
    if (pCrtcVrr_ && wantVrr != vrrActive_) {
        waitForFlip();
        drmModeAtomicReq* req = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(req, crtcId_, pCrtcVrr_, wantVrr ? 1 : 0);
        const int r = drmModeAtomicCommit(fd_, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
        drmModeAtomicFree(req);
        if (r == 0) vrrActive_ = wantVrr;
        else lastError_ = std::string("VRR_ENABLED change failed: ") + std::strerror(-r);
        if (r != 0) modeIdx_ = vrrActive_ ? kModeVrr : kModeFixed;
    }
    if (p.pattern != basePattern_) {
        basePattern_ = p.pattern;
        rebuildBase();
        for (auto& b : bufs_) b.full = true;
    }

    for (int i = 0; i < 3; ++i)
        if (i != displayed_ && i != pending_) {
            back_ = i;
            break;
        }
    Buffer& b = bufs_[back_];

    std::vector<Rect> rects;
    std::vector<Rect> dyn;
    if (p.pattern == kPatMoving) {
        double wx, bx;
        moveBars(moveBarX(p.patternTime, width_), width_, wx, bx);
        for (double x : {wx, bx}) {
            const int xa = static_cast<int>(std::floor(x)), xb = static_cast<int>(std::ceil(x + kMoveBarWidth));
            dyn.push_back({xa, 0, xb - xa + 1, height_});
        }
    }
    if (p.frameCounter) {
        Rect c;
        counterRect(width_, c.x, c.y, c.w, c.h);
        dyn.push_back(c);
    }
    const Rect ovRect = p.overlayVisible ? Rect{p.overlayX, p.overlayY, p.overlayW, p.overlayH} : Rect{};
    if (b.full) {
        rects.push_back({0, 0, width_, height_});
        b.full = false;
    } else {
        rects = b.dynamic;  // restore whatever moved since this buffer was last drawn
        rects.insert(rects.end(), dyn.begin(), dyn.end());
        const bool ovNeeded = p.overlayVisible ? (b.overlayVersion != p.overlayVersion || !b.overlayShown)
                                               : b.overlayShown;
        if (ovNeeded) {
            // The old area is restored first (skipped when it is the same rect), then the new one is drawn.
            const Rect& o = b.overlayRect;
            if (o.x != ovRect.x || o.y != ovRect.y || o.w != ovRect.w || o.h != ovRect.h) rects.push_back(o);
            rects.push_back(ovRect);
        }
    }
    b.overlayRect = ovRect;
    b.overlayVersion = p.overlayVersion;
    b.overlayShown = p.overlayVisible;
    b.dynamic = dyn;
    for (const Rect& r : rects) {
        const bool isOverlay = r.x == ovRect.x && r.y == ovRect.y && r.w == ovRect.w && r.h == ovRect.h;
        if (!(isOverlay && blitOverlay(b, r, p))) compose(b, r, p);
    }

    // The previous flip must complete before the next commit can be queued.
    waitForFlip();
    return RenderResult::Rendered;
}

PresentOutcome KmsBackend::present(uint64_t frameIndex) {
    drmModeAtomicReq* req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, planeId_, pFb_, bufs_[back_].fbId);
    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    if (modeIdx_ == kModeAsync) flags |= DRM_MODE_PAGE_FLIP_ASYNC;
    const int r = drmModeAtomicCommit(fd_, req, flags, reinterpret_cast<void*>(static_cast<uintptr_t>(frameIndex)));
    drmModeAtomicFree(req);
    PresentOutcome o;
    o.ok = r == 0;
    o.status = errnoName(-r);
    if (r == 0) pending_ = back_;
    return o;
}

void KmsBackend::pollTiming(std::vector<DisplayTimingSample>& out) {
    handleEvents(0);
    out.insert(out.end(), flips_.begin(), flips_.end());
    flips_.clear();
}

void KmsBackend::restore() {
    waitForFlip();
    if (pCrtcVrr_) {
        drmModeAtomicReq* req = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(req, crtcId_, pCrtcVrr_, savedVrr_);
        drmModeAtomicCommit(fd_, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
        drmModeAtomicFree(req);
    }
    if (savedCrtc_ && savedCrtc_->mode_valid && savedCrtc_->buffer_id)
        drmModeSetCrtc(fd_, savedCrtc_->crtc_id, savedCrtc_->buffer_id, savedCrtc_->x, savedCrtc_->y, &connId_, 1,
                       &savedCrtc_->mode);
}

DisplayInfo KmsBackend::displayInfo() {
    DisplayInfo d;
    d.outputName = connName_ + " (" + cardPath_ + ")";
    d.monitorName = monitorName_;
    d.width = width_;
    d.height = height_;
    d.refreshHz = modeRefresh(mode_);
    d.extra = std::string("vrr_capable=") + (vrrCapable_ ? "1" : "0") + "  VRR_ENABLED=" +
              (pCrtcVrr_ ? (vrrActive_ ? "1" : "0") : "n/a") + "  max bpc=" + std::to_string(maxBpc_);
    if (!edidRange_.empty()) d.extra += "  " + edidRange_;
    return d;
}

BackendInfo KmsBackend::info() {
    BackendInfo b;
    b.api = "DRM/KMS atomic page flips (dumb buffers, CPU drawn, no compositor)";
    b.gpu = driverName_ + " " + cardPath_;
    b.swapFormat = is10_ ? "DRM_FORMAT_XRGB2101010 (10-bit SDR)" : "DRM_FORMAT_XRGB8888 (8-bit SDR)";
    if (want10_ != is10_) b.swapFormat += " (change pending)";
    b.presentMode = presentModes()[modeIdx_].name;
    b.tearing = asyncSupported_ ? "atomic async page flips supported (DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP)"
                                : "atomic async page flips NOT supported by this kernel/driver";
    b.timingSource = "KMS page-flip event timestamps (kernel vblank time, CLOCK_MONOTONIC)";
    b.timingAvailable = lastFlipNs_ != 0 && nowNs() - lastFlipNs_ < 2000000000LL;
    b.width = width_;
    b.height = height_;
    if (!pCrtcVrr_) b.notes.push_back("CRTC has no VRR_ENABLED property: driver cannot do VRR here");
    if (!vrrCapable_) b.notes.push_back("Connector reports vrr_capable=0: kernel does not consider this sink VRR-capable");
    if (!lastError_.empty()) b.notes.push_back("Last error: " + lastError_);
    b.notes.push_back("Flip timestamps show when the kernel latched our flips, not repeated (LFC) scanouts.");
    return b;
}

std::vector<PresentModeInfo> KmsBackend::presentModes() const {
    std::vector<PresentModeInfo> v;
    v.push_back({"vrr", pCrtcVrr_ ? "Atomic flip; VRR_ENABLED=1 (adaptive sync)" : "Atomic flip (no VRR_ENABLED prop!)",
                 true});
    v.push_back({"fixed", "Atomic flip; VRR_ENABLED=0 (fixed refresh control test)", true});
    if (asyncSupported_) v.push_back({"async", "Atomic ASYNC flip (tearing); VRR_ENABLED=1", false});
    return v;
}

void KmsBackend::setPresentMode(int index) {
    if (index < 0 || index >= static_cast<int>(presentModes().size())) return;
    modeIdx_ = index;
}

}  // namespace vrr
