#include "core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sys/time.h>

namespace zk {

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::atomic<LogLevel> g_level{std::getenv("ZK_LOG_DEBUG") ? LogLevel::Debug : LogLevel::Info};

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

}  // namespace

void setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    if (!path.empty()) g_file = fopen(path.c_str(), "a");
}

void setLogLevel(LogLevel minimum) { g_level = minimum; }

LogLevel logLevel() { return g_level; }

void log(LogLevel level, const char* fmt, ...) {
    if (level < g_level.load()) return;
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);

    timeval tv;
    gettimeofday(&tv, nullptr);
    tm local;
    localtime_r(&tv.tv_sec, &local);
    char stamp[16];
    snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
             static_cast<int>(tv.tv_usec / 1000));

    std::lock_guard<std::mutex> lock(g_mutex);
    fprintf(stderr, "%s %s %s\n", stamp, levelName(level), message);
    if (g_file) {
        fprintf(g_file, "%s %s %s\n", stamp, levelName(level), message);
        fflush(g_file);
    }
}

}  // namespace zk
