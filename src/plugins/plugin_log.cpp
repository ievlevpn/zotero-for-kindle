#include "plugins/plugin_log.h"

#include <ctime>
#include <filesystem>
#include <system_error>

#include "core/log.h"

namespace zk::plugins {

namespace fs = std::filesystem;

void hostLog(const std::string& text) { zk::log(zk::LogLevel::Error, "%s", text.c_str()); }

PluginLog::PluginLog(std::string path, size_t maxBytes, std::function<int64_t()> clock)
    : path_(std::move(path)), max_(maxBytes), clock_(std::move(clock)) {}

PluginLog::~PluginLog() {
    if (f_) std::fclose(f_);
}

bool PluginLog::open() {
    if (f_) return true;
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    f_ = std::fopen(path_.c_str(), "ab");
    if (!f_) return false;
    auto size = fs::file_size(path_, ec);
    size_ = ec ? 0 : static_cast<size_t>(size);
    return true;
}

std::string PluginLog::write(LogLevel level, std::string text) {
    if (text.size() > kMaxEntry) {
        size_t cut = kMaxEntry;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;  // whole characters
        text = text.substr(0, cut) + " … (cut)";
    }
    time_t secs = static_cast<time_t>(clock_() / 1000);
    struct tm tm {};
    localtime_r(&secs, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    std::string line = std::string(stamp) + (level == LogLevel::Warn ? " [warn] " : level == LogLevel::Error ? " [error] " : " ") + text;

    if (!open()) return line;
    if (size_ > 0 && size_ + line.size() + 1 > max_) {
        std::fclose(f_);
        f_ = nullptr;
        std::error_code ec;
        fs::rename(path_, path_ + ".1", ec);
        if (!open()) return line;
    }
    std::fwrite(line.data(), 1, line.size(), f_);
    std::fputc('\n', f_);
    std::fflush(f_);
    size_ += line.size() + 1;
    return line;
}

}  // namespace zk::plugins
