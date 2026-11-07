#include "sync/config.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace zk {

using nlohmann::json;

namespace {

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<Config> parse(const std::string& text) {
    auto j = json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string{}; };
    Config c;
    if (j.contains("userId") && j["userId"].is_number_integer()) c.userId = j["userId"].get<int64_t>();
    else if (std::string s = str("userId"); !s.empty()) c.userId = std::strtoll(s.c_str(), nullptr, 10);
    c.apiKey = str("apiKey");
    c.webdavUrl = str("webdavUrl");
    c.webdavUser = str("webdavUser");
    c.webdavPassword = str("webdavPassword");
    if (c.userId <= 0 || c.apiKey.empty()) return std::nullopt;  // WebDAV is optional; the account is not
    return c;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

}  // namespace

std::optional<Config> loadConfig(const std::string& dir) {
    auto text = readFile(dir + "/config.json");
    return text ? parse(*text) : std::nullopt;
}

bool saveConfig(const std::string& dir, const Config& c) {
    const json j = {{"userId", c.userId}, {"apiKey", c.apiKey}, {"webdavUrl", c.webdavUrl},
                    {"webdavUser", c.webdavUser}, {"webdavPassword", c.webdavPassword}};
    const std::string text = j.dump(2) + "\n";
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        log(LogLevel::Error, "config: can't create %s: %s", dir.c_str(), std::strerror(errno));
        return false;
    }
    const std::string path = dir + "/config.json";
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        log(LogLevel::Error, "config: can't write %s: %s", tmp.c_str(), std::strerror(errno));
        return false;
    }
    ::fchmod(fd, 0600);  // in case the file already existed with other permissions
    const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        log(LogLevel::Error, "config: can't save %s: %s", path.c_str(), std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool importDroppedConfig(const std::string& droppedPath, const std::string& dir) {
    auto text = readFile(droppedPath);
    if (!text) return false;
    auto c = parse(*text);
    if (!c) {
        log(LogLevel::Warn, "config: %s is missing userId or apiKey; left in place", droppedPath.c_str());
        return false;
    }
    if (!saveConfig(dir, *c)) return false;
    if (::unlink(droppedPath.c_str()) != 0)
        log(LogLevel::Warn, "config: imported, but can't delete %s: %s", droppedPath.c_str(), std::strerror(errno));
    log(LogLevel::Info, "config: imported %s", droppedPath.c_str());
    return true;
}

std::optional<Config> configFromEnvFile(const std::string& path) {
    auto text = readFile(path);
    if (!text) return std::nullopt;
    Config c;
    std::istringstream lines(*text);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("export ", 0) == 0) line = trim(line.substr(7));
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        if (key == "ZK_WEBDAV_URL") c.webdavUrl = value;
        else if (key == "ZK_WEBDAV_USER") c.webdavUser = value;
        else if (key == "ZK_WEBDAV_PASSWORD") c.webdavPassword = value;
        else if (key == "ZK_ZOTERO_USER_ID") c.userId = std::strtoll(value.c_str(), nullptr, 10);
        else if (key == "ZK_ZOTERO_API_KEY") c.apiKey = value;
    }
    return c;
}

}  // namespace zk
