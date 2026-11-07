#include "platform/kindle/wet_ink.h"

#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>

#include <csignal>
#include <cerrno>
#include <cstring>
#include <ctime>

#include "core/log.h"

namespace zk {

namespace {

constexpr key_t kSemKey = 0x537a70;    // 13 semaphores, owned by WetInkHandler
constexpr key_t kQueueKey = 0x537a6f;  // messages to the handler (and its type 15 replies)
constexpr int kSemCount = 13;

// Semaphores: client -> handler.
constexpr int kWake = 0, kConnect = 2, kDisconnect = 4, kStrokeDone = 6, kContextDone = 9, kMessages = 10;
// Handler -> client.
constexpr int kNews = 1, kConnected = 3, kStroke = 5, kContext = 8, kIdle = 11;

// Messages (queue mtype).
constexpr long kSetMode = 1, kSetCanvas = 6, kSetWindow = 7, kSetParam = 8, kSetScale = 9, kReply = 15;
constexpr uint8_t kModePen = 0x00, kModeDisabled = 0x0a, kModeOff = 0x0b;

// The live session, for emergencyOff (-1: none).
volatile sig_atomic_t gSem = -1, gQueue = -1;

void onFatalSignal(int sig) {
    WetInk::emergencyOff();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

}  // namespace

void WetInk::emergencyOff() {
    const int sem = gSem, queue = gQueue;
    if (sem < 0 || queue < 0) return;
    gSem = gQueue = -1;
    struct {
        long type;
        unsigned char mode;
    } m{kSetMode, kModeOff};
    msgsnd(queue, &m, 1, IPC_NOWAIT);
    for (int num : {kMessages, kWake, kDisconnect, kWake}) {
        sembuf op{static_cast<unsigned short>(num), 1, IPC_NOWAIT};
        semop(sem, &op, 1);
    }
}

std::unique_ptr<WetInk> WetInk::open() {
    const int sem = semget(kSemKey, kSemCount, 0666);
    const int queue = msgget(kQueueKey, 0666);
    if (sem < 0 || queue < 0) return nullptr;
    return std::unique_ptr<WetInk>(new WetInk(sem, queue));
}

WetInk::~WetInk() { disconnect(); }

bool WetInk::post(int num, int delta) {
    sembuf op{static_cast<unsigned short>(num), static_cast<short>(delta), IPC_NOWAIT};
    return semop(sem_, &op, 1) == 0;
}

bool WetInk::take(int num, int timeoutMs) {
    sembuf op{static_cast<unsigned short>(num), -1, static_cast<short>(timeoutMs ? 0 : IPC_NOWAIT)};
    if (!timeoutMs) return semop(sem_, &op, 1) == 0;
    timespec ts{timeoutMs / 1000, (timeoutMs % 1000) * 1000000L};
    return semtimedop(sem_, &op, 1, &ts) == 0;
}

bool WetInk::send(long type, const void* data, size_t n) {
    struct {
        long type;
        unsigned char data[64];
    } m{};
    m.type = type;
    std::memcpy(m.data, data, n);
    if (msgsnd(queue_, &m, n, IPC_NOWAIT) == 0) return true;
    log(LogLevel::Warn, "wet ink: message %ld: %s", type, std::strerror(errno));
    return false;
}

bool WetInk::sendMode(uint8_t m) {
    const bool ok = send(kSetMode, &m, 1);
    post(kMessages, 1);
    post(kWake, 1);
    return ok;
}

bool WetInk::handshake() {
    post(kConnect, 1);
    post(kWake, 1);
    if (take(kConnected, 1500)) return true;
    take(kConnect, 0);  // nobody answered: withdraw the request
    return false;
}

bool WetInk::connect(unsigned long window, int canvasW, int canvasH) {
    if (connected_) disconnect();
    if (!handshake()) {
        // A session a crashed run left open makes the handler ignore new requests: end it, retry.
        post(kDisconnect, 1);
        post(kWake, 1);
        if (!handshake()) {
            log(LogLevel::Warn, "wet ink: the handler did not answer");
            return false;
        }
    }
    connected_ = true;
    gSem = sem_;
    gQueue = queue_;
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGSEGV, SIGABRT, SIGBUS}) std::signal(sig, onFatalSignal);
    stop_ = false;
    acker_ = std::thread([this] { ackLoop(); });

    // The notebook's setup, in its order: disabled while configuring, window, thickness, canvas,
    // scale; the caller then picks the mode.
    uint8_t disabled = kModeDisabled;
    send(kSetMode, &disabled, 1);
    const uint32_t id = static_cast<uint32_t>(window);
    send(kSetWindow, &id, 4);
    unsigned char thickness[5] = {0};
    const float t = 3.5f;
    std::memcpy(thickness + 1, &t, 4);
    send(kSetParam, thickness, 5);
    const uint16_t canvas[4] = {0, 0, static_cast<uint16_t>(canvasW), static_cast<uint16_t>(canvasH)};
    send(kSetCanvas, canvas, 8);
    const float scale[2] = {1.0f, 1.0f};
    send(kSetScale, scale, 8);
    post(kMessages, 1);
    post(kWake, 1);
    mode_ = Mode::Paused;
    log(LogLevel::Info, "wet ink: connected, window 0x%lx, canvas %dx%d", window, canvasW, canvasH);
    return true;
}

void WetInk::setMode(Mode m) {
    if (!connected_ || m == mode_) return;
    if (sendMode(m == Mode::Pen ? kModePen : kModeDisabled)) mode_ = m;  // else retried next time
}

void WetInk::disconnect() {
    if (!connected_) return;
    gSem = gQueue = -1;
    sendMode(kModeOff);
    post(kDisconnect, 1);
    post(kWake, 1);
    stop_ = true;
    if (acker_.joinable()) acker_.join();
    connected_ = false;
    log(LogLevel::Info, "wet ink: disconnected");
}

// The handler waits for each stroke (and stroke context) to be taken before it moves on; we don't
// need its copy (the app reads the pen itself), so every one is acknowledged at once.
void WetInk::ackLoop() {
    struct {
        long type;
        unsigned char data[512];
    } reply;
    while (!stop_) {
        take(kNews, 200);
        while (take(kStroke, 0)) {
            post(kStrokeDone, 1);
            post(kWake, 1);
        }
        while (take(kContext, 0)) {
            post(kContextDone, 1);
            post(kWake, 1);
        }
        while (take(kIdle, 0)) {
        }
        while (take(kConnected, 0)) {
        }
        // Its brush-parameter replies would otherwise fill the queue.
        while (msgrcv(queue_, &reply, sizeof reply.data, kReply, IPC_NOWAIT | MSG_NOERROR) >= 0) {
        }
    }
}

}  // namespace zk
