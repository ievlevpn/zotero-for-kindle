#pragma once
// Plugin folders for host tests, written into a fresh temp dir from inline JavaScript.

#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "plugins/host.h"

namespace zk::test {

class FakePlugins {
public:
    FakePlugins() {
        static int n = 0;
        dir_ = std::filesystem::temp_directory_path() /
               ("zk-host-" + std::to_string(::getpid()) + "-" + std::to_string(++n) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir_);
    }
    ~FakePlugins() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    FakePlugins(const FakePlugins&) = delete;
    FakePlugins& operator=(const FakePlugins&) = delete;

    // A plugin folder <id>/ with plugin.json (name = `name`, or the id capitalised) and main.js.
    void add(const std::string& id, const std::string& js, const std::vector<std::string>& permissions = {},
             const std::string& name = "") {
        std::string perms;
        for (const auto& p : permissions) perms += (perms.empty() ? "\"" : ",\"") + p + "\"";
        std::string shown = name;
        if (shown.empty()) {
            shown = id;
            shown[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(shown[0])));
        }
        write(id + "/plugin.json", "{\"id\":\"" + id + "\",\"name\":\"" + shown +
                                       "\",\"version\":\"1.0.0\",\"api\":0,\"main\":\"main.js\",\"permissions\":[" +
                                       perms + "]}");
        write(id + "/main.js", js);
    }

    void write(const std::string& rel, const std::string& text) {
        auto p = dir_ / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
    }

    std::string read(const std::string& rel) const {
        std::ifstream in(dir_ / rel, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    bool exists(const std::string& rel) const { return std::filesystem::exists(dir_ / rel); }
    std::string log(const std::string& id) const { return read(id + "/plugin.log"); }

    std::string dir() const { return dir_.string(); }
    std::string path(const std::string& rel) const { return (dir_ / rel).string(); }

    // Deps for a host over this folder: small budgets keep the tests fast.
    PluginHostDeps deps() const {
        PluginHostDeps d;
        d.pluginsDir = dir();
        d.crashMarker = path(".running");
        d.appVersion = Version{9, 9, 9};
        d.handlerBudgetMs = 300;
        d.backgroundBudgetMs = 600;
        d.minTimerMs = 0;  // scaled down with the budgets (the floor's own test sets it)
        return d;
    }

private:
    std::filesystem::path dir_;
};

// Polls `cond` until it holds or `ms` pass.
inline bool eventually(const std::function<bool()>& cond, int ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

inline const PluginStatus* statusOf(const std::vector<PluginStatus>& all, const std::string& id) {
    for (const auto& s : all)
        if (s.folder.dir == id) return &s;
    return nullptr;
}

// A plugin folder with a whole manifest (version 1.2.0, an author, a description); `perms` is the
// inside of the JSON array, `extra` more members (",\"minApp\":\"99.0.0\"").
inline void addPlugin(FakePlugins& d, const std::string& id, const std::string& name, const std::string& perms,
                      const std::string& extra = "", const std::string& js = "") {
    d.write(id + "/plugin.json", "{\"id\":\"" + id + "\",\"name\":\"" + name +
                                   "\",\"version\":\"1.2.0\",\"api\":0,\"main\":\"main.js\",\"author\":\"Ann Author\","
                                   "\"description\":\"Sends new highlights to Readwise and tags what it sent.\","
                                   "\"permissions\":[" + perms + "]" + extra + "}");
    d.write(id + "/main.js", js);
}

// What the Settings → Plugins goldens show: two on (one with a badge and an old error), one the
// app turned off, one new, one incompatible, one broken folder.
inline void addShowcasePlugins(FakePlugins& d) {
    addPlugin(d, "feed", "Feed Riffle",
              "\"library:read\",\"annotations:read\",\"net:readwise.io\",\"net:*.example.org\",\"zotero:tags\",\"zotero:remove\","
              "\"background\"");
    addPlugin(d, "arxiv", "arXiv Watch", "\"library:read\",\"net:export.arxiv.org\",\"zotero:create-items\"");
    addPlugin(d, "aged", "Citation Counts", "\"library:read\"", ",\"minApp\":\"99.0.0\"");
    addPlugin(d, "tired", "Word Counter", "\"fulltext:read\"");
    addPlugin(d, "fresh", "Reading Streaks", "\"annotations:read\"");
    d.write("broken/readme.txt", "no manifest here");
    d.write(".state.json",
            R"J({"plugins":{"tired":{"enabled":false,"everEnabled":true,"stopped":true,"failures":3,
                "errors":["TypeError: cannot read property 'length' of undefined (main.js:41)","Timed out after 5 s while handling highlightCreated","Timed out after 5 s while handling syncFinished"]},
                "feed":{"enabled":true,"everEnabled":true,"badge":"12 highlights sent today",
                "errors":["fetch to readwise.io failed: HTTP 502"]},
                "arxiv":{"enabled":true,"everEnabled":true}}})J");
}

inline std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += s + "\n";
    return out;
}

}  // namespace zk::test
