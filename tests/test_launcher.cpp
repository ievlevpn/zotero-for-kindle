// assets/launcher/Zotero.sh, run under sh against a temporary Kindle: the update protocol
// (format 1, src/update/install.h) with stand-in apps that record their runs.
#include <doctest/doctest.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& text, bool exec = false) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
    if (exec) fs::permissions(p, fs::perms::owner_all);
}

// A stand-in bin/zotero: records "<version> <exit>" in $ZK_HOME/runs and exits with `code`;
// `--reset-system-ink` (after an unclean exit) is recorded in $ZK_HOME/resets instead.
// With `installs`, it first does what the app does after a verified download, if the test left
// an "install-me" note: backups, the swap, state=installed, then exit 75.
std::string app(const std::string& version, int code, bool installs = false) {
    std::string s = "#!/bin/sh\n[ \"$1\" = --reset-system-ink ] && { echo reset >> resets; exit 0; }\n";
    if (installs)
        s += "if [ -f install-me ]; then rm install-me\n"
             "  mkdir -p update/old && cp -r bin lib share update/old/ && cp \"$ZK_LAUNCHER\" update/old/Zotero.sh\n"
             "  cp zotero.db update/old/zotero.db && cp \"$ZK_CONFIG\" \"$ZK_CONFIG.old\"\n"
             "  echo 'installing 0.2.0 0.3.0' > update/state\n"
             "  rm -rf bin lib share && cp -r update/new/bin update/new/lib update/new/share .\n"
             "  echo 'db 0.3.0' > zotero.db && echo 'config 0.3.0' > \"$ZK_CONFIG\"\n"
             "  echo 'installed 0.2.0 0.3.0' > update/state\n"
             "  echo \"" + version + " 75\" >> runs; exit 75; fi\n";
    s += "echo \"" + version + " " + std::to_string(code) + "\" >> runs\nexit " + std::to_string(code) + "\n";
    return s;
}

struct Kindle {
    fs::path root, home;
    Kindle(const std::string& oldApp = app("0.2.0", 0)) {
        char tmpl[] = "/tmp/zk-launcher-XXXXXX";
        root = mkdtemp(tmpl);
        home = root / "zotero";
        writeFile(home / "bin/zotero", oldApp, true);
        writeFile(home / "lib/libzk-pdf.so", "pdf 0.2.0");
        writeFile(home / "share/cacert.pem", "ca 0.2.0");
        writeFile(home / "zotero.db", "db 0.2.0");
        writeFile(root / "var/config.json", "config 0.2.0");
        writeFile(root / "documents/Zotero.sh", "launcher 0.2.0");
        fs::copy_file("assets/launcher/Zotero.sh", root / "documents/Zotero.sh.src");
    }
    ~Kindle() { fs::remove_all(root); }
    // A complete backup of 0.2.0 and 0.3.0 swapped in, as installRelease leaves them.
    void installed(const std::string& word, const std::string& newApp) {
        fs::create_directories(home / "update/old");
        for (const char* d : {"bin", "lib", "share"}) fs::copy(home / d, home / "update/old" / d, fs::copy_options::recursive);
        fs::copy_file(root / "documents/Zotero.sh", home / "update/old/Zotero.sh");
        fs::copy_file(home / "zotero.db", home / "update/old/zotero.db");
        fs::copy_file(root / "var/config.json", root / "var/config.json.old");
        writeFile(home / "bin/zotero", newApp, true);
        writeFile(home / "lib/libzk-pdf.so", "pdf 0.3.0");
        writeFile(home / "zotero.db", "db 0.3.0");
        writeFile(home / "zotero.db-wal", "wal 0.3.0");
        writeFile(root / "var/config.json", "config 0.3.0");
        writeFile(root / "documents/Zotero.sh", "launcher 0.3.0");
        writeFile(home / "update/state", word + " 0.2.0 0.3.0\n");
    }
    int launch() {
        // A `sync` on PATH that records what update/state held when it ran.
        writeFile(root / "fakebin/sync", "#!/bin/sh\necho \"sync $(cat '" + (home / "update/state").string() +
                                             "' 2>/dev/null)\" >> '" + (home / "syncs").string() + "'\n", true);
        const std::string cmd = "cd '" + fs::current_path().string() + "' && PATH='" + (root / "fakebin").string() +
                                ":'\"$PATH\" ZK_HOME='" + home.string() +
                                "' ZK_LAUNCHER='" + (root / "documents/Zotero.sh").string() + "' ZK_CONFIG='" +
                                (root / "var/config.json").string() + "' ZK_DISPLAY=none ZK_INK_MARK='" +
                                (root / "ink-session").string() + "' sh '" +
                                (root / "documents/Zotero.sh.src").string() + "'";
        return std::system(cmd.c_str());
    }
    std::string runs() const { return readFile(home / "runs"); }
    std::string file(const fs::path& rel) const { return readFile(root / rel); }
    void checkRolledBack() const {
        CHECK(file("zotero/bin/zotero") == app("0.2.0", 0));
        CHECK(file("zotero/lib/libzk-pdf.so") == "pdf 0.2.0");
        CHECK(file("zotero/share/cacert.pem") == "ca 0.2.0");
        CHECK(file("zotero/zotero.db") == "db 0.2.0");
        CHECK_FALSE(fs::exists(home / "zotero.db-wal"));
        CHECK(file("var/config.json") == "config 0.2.0");
        CHECK((fs::status(root / "var/config.json").permissions() & (fs::perms::group_all | fs::perms::others_all)) ==
              fs::perms::none);
        CHECK_FALSE(fs::exists(root / "var/config.json.old"));
        CHECK(file("documents/Zotero.sh") == "launcher 0.2.0");
        CHECK(file("zotero/update/rolled-back") == "0.2.0 0.3.0\n");
        CHECK_FALSE(fs::exists(home / "update/state"));
        CHECK_FALSE(fs::exists(home / "update/old"));
    }
};

}  // namespace

TEST_CASE("launcher: a normal run starts the app once") {
    Kindle k;
    CHECK(k.launch() == 0);
    CHECK(k.runs() == "0.2.0 0\n");
    CHECK(fs::exists(k.home / "log/zotero.log"));
}

TEST_CASE("launcher: an app that lost its execute bit still starts") {
    Kindle k;
    fs::permissions(k.home / "bin/zotero", fs::perms::owner_read | fs::perms::owner_write);
    k.launch();
    CHECK(k.runs() == "0.2.0 0\n");
}

TEST_CASE("launcher: exit 75 starts the new version; one that doesn't confirm is rolled back at once") {
    Kindle k(app("0.2.0", 0, true));
    writeFile(k.home / "install-me", "");
    writeFile(k.home / "update/new/bin/zotero", app("0.3.0", 1), true);  // 0.3.0 crashes before confirming
    writeFile(k.home / "update/new/lib/libzk-pdf.so", "pdf 0.3.0");
    writeFile(k.home / "update/new/share/cacert.pem", "ca 0.3.0");
    k.launch();
    // 0.2.0 installs and asks for a restart; 0.3.0 fails; the launcher restores 0.2.0 and starts it.
    CHECK(k.runs() == "0.2.0 75\n0.3.0 1\n0.2.0 0\n");
    CHECK(k.file("zotero/bin/zotero") == app("0.2.0", 0, true));
    CHECK(k.file("zotero/lib/libzk-pdf.so") == "pdf 0.2.0");
    CHECK(k.file("zotero/zotero.db") == "db 0.2.0");
    CHECK(k.file("var/config.json") == "config 0.2.0");
    CHECK(k.file("zotero/update/rolled-back") == "0.2.0 0.3.0\n");
    CHECK_FALSE(fs::exists(k.home / "update/state"));
}

TEST_CASE("launcher: a new version that confirms stays") {
    Kindle k;
    // 0.3.0 confirms (deletes update/state and the backups) and exits normally.
    k.installed("installed", "#!/bin/sh\necho \"0.3.0 0\" >> runs\nrm -rf update/state update/old\nexit 0\n");
    k.launch();
    CHECK(k.runs() == "0.3.0 0\n");
    CHECK(k.file("zotero/lib/libzk-pdf.so") == "pdf 0.3.0");
    CHECK_FALSE(fs::exists(k.home / "update/rolled-back"));
}

TEST_CASE("launcher: installed becomes tried before the new version runs") {
    Kindle k;
    k.installed("installed", "#!/bin/sh\ncat update/state >> runs\nexit 0\n");
    k.launch();
    CHECK(k.runs().rfind("tried 0.2.0 0.3.0\n", 0) == 0);
}

TEST_CASE("launcher: tried means roll back") {
    Kindle k;
    k.installed("tried", app("0.3.0", 0));
    k.launch();
    CHECK(k.runs() == "0.2.0 0\n");
    k.checkRolledBack();
}

TEST_CASE("launcher: installing means an interrupted swap is rolled back") {
    Kindle k;
    k.installed("installing", app("0.3.0", 0));
    fs::remove_all(k.home / "share");  // the power went off mid-swap
    k.launch();
    CHECK(k.runs() == "0.2.0 0\n");
    k.checkRolledBack();
}

TEST_CASE("launcher: an interrupted rollback finishes on the next start") {
    Kindle k;
    k.installed("tried", app("0.3.0", 0));
    fs::remove_all(k.home / "bin");  // a rollback that stopped after deleting bin
    k.launch();
    CHECK(k.runs() == "0.2.0 0\n");
    k.checkRolledBack();
}

TEST_CASE("launcher: after an unclean exit it resets the system ink, not after a normal one") {
    {
        Kindle k(app("0.2.0", 137));  // killed (SIGKILL) with a wet-ink session open
        writeFile(k.root / "ink-session", "");
        k.launch();
        CHECK(k.runs() == "0.2.0 137\n");
        CHECK(readFile(k.home / "resets") == "reset\n");
    }
    {
        // No session was open (any Kindle without the pen, or an older app after a rollback,
        // which would take the flag for a normal start): the app isn't started again.
        Kindle k(app("0.2.0", 137));
        k.launch();
        CHECK_FALSE(fs::exists(k.home / "resets"));
        CHECK(k.runs() == "0.2.0 137\n");
    }
    {
        Kindle k;
        k.launch();
        CHECK_FALSE(fs::exists(k.home / "resets"));
    }
}

TEST_CASE("launcher: endless 75s stop after five") {
    Kindle k(app("0.2.0", 75));
    k.launch();
    CHECK(k.runs() == "0.2.0 75\n0.2.0 75\n0.2.0 75\n0.2.0 75\n0.2.0 75\n");
}

TEST_CASE("launcher fix: syncs before it drops or advances the state") {
    Kindle k;
    k.installed("tried", app("0.3.0", 0));
    k.launch();
    k.checkRolledBack();
    // The restored files reached the disk while update/state still said "tried".
    CHECK(readFile(k.home / "syncs").find("sync tried 0.2.0 0.3.0\n") != std::string::npos);
}

TEST_CASE("launcher fix: installed becomes tried on disk before the new version runs") {
    Kindle k;
    k.installed("installed", "#!/bin/sh\necho \"0.3.0 0\" >> runs\nrm -rf update/state update/old\nexit 0\n");
    k.launch();
    CHECK(readFile(k.home / "syncs").find("sync tried 0.2.0 0.3.0\n") != std::string::npos);
    CHECK_FALSE(fs::exists(k.home / "update/state.tmp"));
}

TEST_CASE("launcher fix: an unreadable state with a backup rolls back") {
    Kindle k;
    k.installed("tried", app("0.3.0", 0));
    writeFile(k.home / "update/state", "");  // a write torn by a power cut
    k.launch();
    CHECK(k.runs() == "0.2.0 0\n");
    CHECK(k.file("zotero/bin/zotero") == app("0.2.0", 0));
    CHECK_FALSE(fs::exists(k.home / "update/state"));
}
