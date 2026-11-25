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
    std::string script = "Zotero.sh.src";  // the launcher's file name, as the scriptlet runner starts it
    int launch() {
        // A `sync` on PATH that records what update/state held when it ran.
        writeFile(root / "fakebin/sync", "#!/bin/sh\necho \"sync $(cat '" + (home / "update/state").string() +
                                             "' 2>/dev/null)\" >> '" + (home / "syncs").string() + "'\n", true);
        const std::string cmd = "cd '" + fs::current_path().string() + "' && PATH='" + (root / "fakebin").string() +
                                ":'\"$PATH\" ZK_HOME='" + home.string() +
                                "' ZK_LAUNCHER='" + (root / "documents/Zotero.sh").string() + "' ZK_CONFIG='" +
                                (root / "var/config.json").string() + "' ZK_DISPLAY=none ZK_INK_MARK='" +
                                (root / "ink-session").string() + "' ZK_PIDFILE='" + pidfile().string() + "' ZK_LOCK='" + lock().string() + "' ZK_PROC='" + (root / "proc").string() + "' sh '" +
                                (root / "documents" / script).string() + "'";
        return std::system(cmd.c_str());
    }
    fs::path pidfile() const { return root / "run/zotero.pid"; }
    fs::path lock() const { return root / "run/zotero.lock"; }
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

namespace {

// The output of a shell command, trailing newline dropped.
std::string shell(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[256];
        while (fgets(buf, sizeof buf, p)) out += buf;
        pclose(p);
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

}  // namespace

TEST_CASE("launcher: while the app runs, another start brings it to the front instead of a second copy, and stays while it runs") {
    Kindle k;
    // A running app, as the launcher sees one: its pid in the pid file, its command line bin/zotero.
    // It notes SIGWINCH, the app's "come to the front" (short sleeps: a trap runs between commands),
    // and ends after about 2 s.
    writeFile(k.home / "bin/zotero-running",
              "#!/bin/sh\ntrap 'echo raised >> raised' WINCH\ni=0\n"
              "while [ $i -lt 20 ]; do sleep 0.1; i=$((i+1)); done\necho ended > ended\n",
              true);
    const std::string pid =
        shell("cd '" + k.home.string() + "' && { sh ./bin/zotero-running >/dev/null 2>&1 & echo $!; }");
    REQUIRE_FALSE(pid.empty());
    writeFile(k.pidfile(), pid + "\n");
    CHECK(k.launch() == 0);
    // The Kindle's library takes a scriptlet that has ended for an app that has closed, and comes
    // back over the window just raised: this launcher returns only once the app has ended.
    CHECK(fs::exists(k.home / "ended"));
    CHECK_FALSE(fs::exists(k.home / "runs"));
    CHECK(readFile(k.home / "log/zotero.log").find("already running") != std::string::npos);
    CHECK(k.file("zotero/raised") == "raised\n");
}

TEST_CASE("launcher: a second start while the first is between runs of the app starts no second copy") {
    // The window where no app runs but a launcher is busy (after an exit 75, during a rollback): the
    // pid file is gone. The stand-in plays that window by removing it and tapping again (once).
    Kindle k(std::string("#!/bin/sh\nif [ ! -f tapped ]; then : > tapped; rm -f \"$ZK_PIDFILE\"\n"
                         "  sh ../documents/Zotero.sh.src; fi\necho '0.2.0 0' >> runs\n"));
    CHECK(k.launch() == 0);
    CHECK(k.runs() == "0.2.0 0\n");
    CHECK(readFile(k.home / "log/zotero.log").find("another launcher") != std::string::npos);
    CHECK_FALSE(fs::exists(k.lock()));  // released at the end
}

TEST_CASE("launcher: a launcher started under another name still holds its lock") {
    Kindle k;
    k.script = "zk-start.sh";  // a runner that copies or renames the scriptlet
    fs::copy_file(k.root / "documents/Zotero.sh.src", k.root / "documents/zk-start.sh");
    // A first launcher, busy between two runs of the app: alive, its command line naming its file.
    const std::string holder = shell("sh -c 'sleep 3 >/dev/null 2>&1 & echo $!'");
    writeFile(k.root / "proc" / holder / "cmdline", std::string("sh\0/mnt/us/documents/zk-start.sh\0", 33));
    fs::create_directories(k.lock().parent_path());
    fs::create_symlink(holder + " zk-start.sh", k.lock());
    CHECK(k.launch() == 0);
    CHECK_FALSE(fs::exists(k.home / "runs"));
    CHECK(readFile(k.home / "log/zotero.log").find("another launcher") != std::string::npos);
    shell("kill " + holder);
}

TEST_CASE("launcher: a lock whose pid now belongs to another program is taken over") {
    Kindle k;
    const std::string other = shell("sh -c 'sleep 3 >/dev/null 2>&1 & echo $!'");
    writeFile(k.root / "proc" / other / "cmdline", std::string("sleep\0" "3\0", 8));
    fs::create_directories(k.lock().parent_path());
    fs::create_symlink(other + " Zotero.sh.src", k.lock());
    CHECK(k.launch() == 0);
    CHECK(k.runs() == "0.2.0 0\n");
    shell("kill " + other);
}

TEST_CASE("launcher: a lock left by a launcher that is gone does not stop a start") {
    Kindle k;
    const std::string gone = shell("sh -c 'echo $$'");
    fs::create_directories(k.lock().parent_path());
    fs::create_symlink(gone, k.lock());
    CHECK(k.launch() == 0);
    CHECK(k.runs() == "0.2.0 0\n");
    CHECK_FALSE(fs::is_symlink(k.lock()));
}

TEST_CASE("launcher: a pid file left by an app that is gone does not stop a start") {
    Kindle k;
    const std::string gone = shell("sh -c 'echo $$'");  // that process has ended
    writeFile(k.pidfile(), gone + "\n");
    CHECK(k.launch() == 0);
    CHECK(k.runs() == "0.2.0 0\n");
}

TEST_CASE("launcher: the pid file holds the app's pid while it runs, and goes when it ends") {
    Kindle k(std::string("#!/bin/sh\necho \"$$ $(cat \"$ZK_PIDFILE\")\" > pids\necho '0.2.0 0' >> runs\n"));
    CHECK(k.launch() == 0);
    const std::string pids = k.file("zotero/pids");
    const auto space = pids.find(' ');
    REQUIRE(space != std::string::npos);
    CHECK(pids.substr(0, space) + "\n" == pids.substr(space + 1));
    CHECK_FALSE(fs::exists(k.pidfile()));
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

TEST_CASE("launcher: an app that ends with an error code leaves the code in the log; a normal end doesn't") {
    {
        Kindle k(app("0.2.0", 139));
        k.launch();
        CHECK(readFile(k.home / "log/zotero.log").find("launcher: the app ended with code 139") != std::string::npos);
    }
    {
        Kindle k;
        k.launch();
        CHECK(readFile(k.home / "log/zotero.log").find("the app ended") == std::string::npos);
    }
}

TEST_CASE("launcher: killed itself while the app runs, it still lets the Kindle sleep") {
    // The system ends the launcher (SIGTERM) along with the app: the app's own signal path can't
    // call lipc, so the launcher's ending is what turns the keep-awake off.
    Kindle k(std::string("#!/bin/sh\nkill -TERM $PPID\nsleep 0.3\necho '0.2.0 0' >> runs\n"));
    writeFile(k.root / "fakebin/lipc-set-prop", "#!/bin/sh\necho \"$*\" >> '" + (k.home / "lipc").string() + "'\n", true);
    k.launch();
    CHECK(readFile(k.home / "lipc") == "com.lab126.powerd preventScreenSaver 0\n");
    CHECK_FALSE(fs::is_symlink(k.lock()));
    for (int i = 0; i < 20 && !fs::exists(k.home / "runs"); i++) usleep(100 * 1000);  // the stand-in's end
}

TEST_CASE("launcher: a second start that finds the app running leaves the keep-awake alone") {
    Kindle k;
    writeFile(k.root / "fakebin/lipc-set-prop", "#!/bin/sh\necho \"$*\" >> '" + (k.home / "lipc").string() + "'\n", true);
    const std::string holder = shell("sh -c 'sleep 3 >/dev/null 2>&1 & echo $!'");
    fs::create_directories(k.lock().parent_path());
    fs::create_symlink(holder, k.lock());
    CHECK(k.launch() == 0);
    CHECK_FALSE(fs::exists(k.home / "runs"));
    CHECK_FALSE(fs::exists(k.home / "lipc"));
    CHECK(fs::read_symlink(k.lock()).string() == holder);  // not ours to release
    shell("kill " + holder);
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

TEST_CASE("launcher: a log over 1 MB becomes zotero.log.1 before the start, replacing the old one") {
    Kindle k;
    const std::string big(1048577, 'x');
    writeFile(k.home / "log/zotero.log", big);
    writeFile(k.home / "log/zotero.log.1", "older");
    CHECK(k.launch() == 0);
    CHECK(k.runs() == "0.2.0 0\n");
    CHECK(k.file("zotero/log/zotero.log.1") == big);
    CHECK(fs::file_size(k.home / "log/zotero.log") < 1024);
}

TEST_CASE("launcher: a log of 1 MB or less is kept as it is") {
    Kindle k;
    const std::string small(1048576, 'x');
    writeFile(k.home / "log/zotero.log", small);
    k.launch();
    CHECK_FALSE(fs::exists(k.home / "log/zotero.log.1"));
    CHECK(k.file("zotero/log/zotero.log").substr(0, small.size()) == small);
}

TEST_CASE("launcher: with accounts, a rollback puts back accounts.json and each account's library it backed up") {
    Kindle k;
    k.installed("tried", app("0.3.0", 0));
    // The account layout: no library or config.json in the old places, none in the backup.
    for (const fs::path& p : {k.home / "zotero.db", k.home / "zotero.db-wal", k.home / "update/old/zotero.db",
                              k.root / "var/config.json", k.root / "var/config.json.old"})
        fs::remove(p);
    writeFile(k.home / "update/old/accounts/111/zotero.db", "db111 0.2.0");
    writeFile(k.home / "update/old/accounts/222/zotero.db", "db222 0.2.0");
    writeFile(k.home / "accounts/111/zotero.db", "db111 0.3.0");
    writeFile(k.home / "accounts/111/zotero.db-wal", "wal111 0.3.0");
    writeFile(k.home / "accounts/111/zotero.db-shm", "shm111 0.3.0");
    writeFile(k.home / "accounts/222/zotero.db", "db222 0.3.0");
    writeFile(k.home / "accounts/222/files/K/book.pdf", "a pdf");
    writeFile(k.home / "accounts/333/zotero.db", "db333 0.3.0");  // not backed up (short of space): stays
    writeFile(k.root / "var/accounts.json", "list 0.3.0");
    writeFile(k.root / "var/accounts.json.old", "list 0.2.0");
    k.launch();
    CHECK(k.runs() == "0.2.0 0\n");
    CHECK(k.file("zotero/bin/zotero") == app("0.2.0", 0));
    CHECK(k.file("zotero/accounts/111/zotero.db") == "db111 0.2.0");
    CHECK_FALSE(fs::exists(k.home / "accounts/111/zotero.db-wal"));
    CHECK_FALSE(fs::exists(k.home / "accounts/111/zotero.db-shm"));
    CHECK(k.file("zotero/accounts/222/zotero.db") == "db222 0.2.0");
    CHECK(k.file("zotero/accounts/222/files/K/book.pdf") == "a pdf");
    CHECK(k.file("zotero/accounts/333/zotero.db") == "db333 0.3.0");
    CHECK(k.file("var/accounts.json") == "list 0.2.0");
    CHECK((fs::status(k.root / "var/accounts.json").permissions() & (fs::perms::group_all | fs::perms::others_all)) ==
          fs::perms::none);
    CHECK_FALSE(fs::exists(k.root / "var/accounts.json.old"));
    CHECK_FALSE(fs::exists(k.home / "zotero.db"));  // nothing made in the old place
    CHECK_FALSE(fs::exists(k.root / "var/config.json"));
    CHECK_FALSE(fs::exists(k.home / "update/old"));
    CHECK(k.file("zotero/update/rolled-back") == "0.2.0 0.3.0\n");
}

TEST_CASE("launcher: a rollback to a version before accounts undoes the new version's move into the account layout") {
    Kindle k;
    k.installed("tried", app("0.3.0", 0));  // backed up by the version before accounts: update/old/zotero.db
    writeFile(k.root / "var/config.json.old", "{\n  \"apiKey\": \"k\",\n  \"userId\": 111\n}\n");
    // What the new version's first start did: the library, the downloads, the plugin data and the
    // credentials moved into account 111.
    for (const fs::path& p : {k.home / "zotero.db", k.home / "zotero.db-wal", k.root / "var/config.json"}) fs::remove(p);
    writeFile(k.home / "plugins/desk/plugin.json", "{}");
    writeFile(k.home / "accounts/111/zotero.db", "db 0.3.0 moved");
    writeFile(k.home / "accounts/111/zotero.db-wal", "wal moved");
    writeFile(k.home / "accounts/111/files/K/book.pdf", "a pdf");
    writeFile(k.home / "accounts/111/plugins/desk/storage.db", "pins");
    writeFile(k.home / "accounts/111/plugins/desk/data/x", "x");
    writeFile(k.home / "accounts/111/plugins/gone/storage.db", "of a plugin since removed");
    writeFile(k.root / "var/accounts.json", "list");
    writeFile(k.root / "var/accounts/111.json", "credentials");
    k.launch();
    CHECK(k.runs() == "0.2.0 0\n");
    CHECK(k.file("zotero/zotero.db") == "db 0.2.0");
    CHECK(k.file("var/config.json") == "{\n  \"apiKey\": \"k\",\n  \"userId\": 111\n}\n");
    // The old version's places again, so it finds its downloads and plugin data...
    CHECK(k.file("zotero/files/K/book.pdf") == "a pdf");
    CHECK(k.file("zotero/plugins/desk/storage.db") == "pins");
    CHECK(k.file("zotero/plugins/desk/data/x") == "x");
    CHECK_FALSE(fs::exists(k.home / "plugins/gone"));
    // ...and the next update moves them again from here: no account list, the moved library put
    // aside (never deleted), so the restored one is the one moved next time.
    CHECK_FALSE(fs::exists(k.root / "var/accounts.json"));
    CHECK_FALSE(fs::exists(k.home / "accounts/111/zotero.db"));
    CHECK_FALSE(fs::exists(k.home / "accounts/111/zotero.db-wal"));
    bool kept = false;
    for (const auto& e : fs::recursive_directory_iterator(k.home / "accounts/.leftovers"))
        if (e.path().filename() == "zotero.db" && readFile(e.path()) == "db 0.3.0 moved") kept = true;
    CHECK(kept);
    CHECK(k.file("zotero/update/rolled-back") == "0.2.0 0.3.0\n");
    CHECK_FALSE(fs::exists(k.home / "update/old"));
}

TEST_CASE("launcher: a rollback to a version with accounts keeps them, a library left in the old place or not") {
    Kindle k;
    k.installed("tried", app("0.3.0", 0));  // update/old/zotero.db: a library 0.2.0 never moved
    writeFile(k.home / "update/old/accounts/111/zotero.db", "db111 0.2.0");
    writeFile(k.home / "accounts/111/zotero.db", "db111 0.3.0");
    writeFile(k.home / "accounts/111/files/K/book.pdf", "a pdf");
    writeFile(k.root / "var/accounts.json", "list 0.3.0");
    writeFile(k.root / "var/accounts.json.old", "list 0.2.0");
    writeFile(k.root / "var/config.json.old", "{\"userId\": 111}");
    k.launch();
    CHECK(k.runs() == "0.2.0 0\n");
    CHECK(k.file("zotero/accounts/111/zotero.db") == "db111 0.2.0");
    CHECK(k.file("zotero/accounts/111/files/K/book.pdf") == "a pdf");
    CHECK_FALSE(fs::exists(k.home / "files"));
    CHECK(k.file("var/accounts.json") == "list 0.2.0");
    CHECK(k.file("zotero/zotero.db") == "db 0.2.0");
}
