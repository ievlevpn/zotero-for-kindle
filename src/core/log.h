#pragma once

#include <string>

namespace zk {

enum class LogLevel { Debug, Info, Warn, Error };

// Writes "HH:MM:SS.mmm LEVEL message" to stderr and, if set, to the log file.
void log(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Appends to `path` from now on; an empty path stops file logging.
void setLogFile(const std::string& path);

}  // namespace zk
