#include "platform/kindle/evdev_pen.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/log.h"

namespace zk {

namespace {

bool hasBit(const unsigned long* bits, int bit) {
    constexpr int kBits = sizeof(unsigned long) * 8;
    return (bits[bit / kBits] >> (bit % kBits)) & 1UL;
}

// A digitizer with a pen tool and absolute X/Y; "stylus" in its name wins (on the Scribe the
// firmware's scaled "stylus-custom" node is preferred over the raw Wacom one).
int score(int fd, std::string& name, bool raw) {
    unsigned long keys[KEY_MAX / (sizeof(unsigned long) * 8) + 1] = {};
    unsigned long abs[ABS_MAX / (sizeof(unsigned long) * 8) + 1] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 || ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) < 0)
        return 0;
    if (!hasBit(keys, BTN_TOOL_PEN) || !hasBit(abs, ABS_X) || !hasBit(abs, ABS_Y)) return 0;
    char buf[128] = "";
    ioctl(fd, EVIOCGNAME(sizeof buf - 1), buf);
    name = buf;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const bool copy = lower.find("custom") != std::string::npos;
    if (raw) return copy ? 1 : 2;
    return lower.find("stylus") != std::string::npos ? 2 : 1;
}

int envInt(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : fallback;
}

}  // namespace

EvdevPen::EvdevPen(int panelW, int panelH, bool raw) {
    std::string name;
    if (const char* forced = std::getenv("ZK_PEN_DEVICE"); forced && *forced) {
        fd_ = ::open(forced, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ >= 0 && score(fd_, name, raw) > 0) path_ = forced;
        else if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    } else if (DIR* d = opendir("/dev/input")) {
        int best = 0;
        while (dirent* e = readdir(d)) {
            if (std::strncmp(e->d_name, "event", 5) != 0) continue;
            const std::string p = std::string("/dev/input/") + e->d_name;
            const int fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            std::string n;
            const int s = score(fd, n, raw);
            if (s > best) {
                if (fd_ >= 0) ::close(fd_);
                fd_ = fd;
                best = s;
                path_ = p;
                name = n;
            } else {
                ::close(fd);
            }
        }
        closedir(d);
    }
    if (fd_ < 0) {
        log(LogLevel::Info, "pen: no stylus digitizer");
        return;
    }
    PenMapping m;
    input_absinfo x{}, y{}, p{};
    ioctl(fd_, EVIOCGABS(ABS_X), &x);
    ioctl(fd_, EVIOCGABS(ABS_Y), &y);
    if (ioctl(fd_, EVIOCGABS(ABS_PRESSURE), &p) == 0) m.maxPressure = p.maximum;
    m.minX = x.minimum;
    m.maxX = x.maximum;
    m.minY = y.minimum;
    m.maxY = y.maximum;
    m.panelW = panelW;
    m.panelH = panelH;
    m.guessSwap();
    m.swapXY = envInt("ZK_PEN_SWAP_XY", m.swapXY) != 0;
    m.flipX = envInt("ZK_PEN_FLIP_X", 0) != 0;
    m.flipY = envInt("ZK_PEN_FLIP_Y", 0) != 0;
    if (const char* t = std::getenv("ZK_PEN_TURNS"); t && *t) {
        m.quarterTurns = std::atoi(t);
        forcedTurns_ = true;
    }
    decoder_.setMapping(m);
    log(LogLevel::Info, "pen: %s (%s) x %d..%d y %d..%d pressure 0..%d panel %dx%d swap %d flip %d/%d",
        path_.c_str(), name.c_str(), m.minX, m.maxX, m.minY, m.maxY, m.maxPressure, panelW, panelH, m.swapXY, m.flipX,
        m.flipY);
}

EvdevPen::~EvdevPen() {
    if (fd_ >= 0) ::close(fd_);
}

void EvdevPen::setView(int quarterTurns, Point origin) {
    PenMapping m = decoder_.mapping();
    if (!forcedTurns_) m.quarterTurns = quarterTurns;
    m.origin = origin;
    decoder_.setMapping(m);
}

void EvdevPen::read(std::vector<InputEvent>& out, int64_t now_ms) {
    if (fd_ < 0) return;
    input_event buf[64];
    for (;;) {
        const ssize_t n = ::read(fd_, buf, sizeof buf);
        if (n <= 0) break;
        for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(input_event); ++i)
            for (const PenEvent& p : decoder_.feed(buf[i].type, buf[i].code, buf[i].value, now_ms)) {
                InputEvent e;
                e.kind = InputEvent::Kind::Pen;
                e.pen = p;
                out.push_back(e);
            }
    }
}

}  // namespace zk
