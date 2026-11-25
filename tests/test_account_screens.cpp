// The account screens (spec: multiple accounts, PIN and Screens): Home's chip and sheet, Settings →
// Accounts, Add account and Update key, the PIN pad (set twice, the wrong-try wait, the forgotten
// PIN's reset-pin.txt), the lock on wake over the reader, the lock at startup, and Remove from this
// Kindle (the open account, the last one) — and their goldens.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "app/account_switch.h"
#include "app/account_ui.h"
#include "app/accounts.h"
#include "app/app.h"
#include "app/device_settings.h"
#include "app/lifecycle.h"
#include "app/pin.h"
#include "app/session.h"
#include "app/startup.h"
#include "screens/account_error.h"
#include "fake_display.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fixture_library.h"
#include "fn_http.h"
#include "golden.h"
#include "platform/scaled_display.h"
#include "screens/account_sheet.h"
#include "screens/accounts.h"
#include "screens/home.h"
#include "screens/pin_pad.h"
#include "screens/reader.h"
#include "screens/remove_account.h"
#include "screens/settings.h"
#include "screens/setup.h"
#include "screens/switching.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/null_sync.h"
#include "test_paths.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/widgets.h"

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int64_t kWall = 1'762'500'000'000;

bool fonts() {
    if (!fs::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

Gesture tapOn(Rect r) { return Gesture{GestureType::Tap, r.center(), r.center()}; }

std::string slurpFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Stands for an account's Home: which account it was made for.
struct AccountHome final : Screen {
    int64_t userId;
    explicit AccountHome(int64_t id) : userId(id) {}
    void draw(Canvas&) override {}
    void onGesture(const Gesture&, Nav&) override {}
};

// A Kindle with accounts on it (Pavel 11, Anna 22 by default), each with a file of its own; the app
// as main wires it: the switcher, the account screens' side, the lock.
struct Rig {
    fs::path dir;
    Paths paths;
    DeviceSettings device{""};
    FakeDisplay display;
    Db emptyDb = Db::open(":memory:");
    Library emptyLib{emptyDb};
    NullSync nullSync;
    FakeKeyboard keyboard;
    std::atomic<int64_t> mono{1'000'000};
    std::atomic<int64_t> wall{kWall};
    App app{display, emptyLib, nullSync, keyboard, [this] { return mono.load(); }};
    std::atomic<bool> online{false};
    std::atomic<bool> listening{false};
    std::unique_ptr<AccountSwitcher> switcher;
    std::unique_ptr<AccountsUi> ui;
    int noAccounts = 0;
    std::function<UserError(const Config&)> ownerAnswer;  // set: what Zotero says of a key's owner

    explicit Rig(std::vector<std::pair<int64_t, std::string>> accounts = {{11, "Pavel"}, {22, "Anna"}}) {
        dir = uniqueTempPath("zk-account-screens");
        fs::remove_all(dir);
        paths.configDir = (dir / "config").string();
        paths.accountsRoot = (dir / "accounts").string();
        paths.pluginsDir = (dir / "plugins").string();
        paths.deviceSettingsFile = (dir / "device.json").string();
        paths.dbPath = (dir / "zotero.db").string();
        paths.filesDir = (dir / "files").string();
        paths.droppedConfig = (dir / "usb-config.json").string();
        fs::create_directories(paths.configDir);
        AccountsState st;
        st.active = accounts.front().first;
        for (const auto& [id, name] : accounts) {
            AccountEntry e;
            e.userId = id;
            e.name = name;
            e.order = static_cast<int>(st.accounts.size());
            st.accounts.push_back(e);
            Config c;
            c.userId = id;
            c.apiKey = "key" + std::to_string(id);
            REQUIRE(saveCredentials(paths, c));
            const fs::path file = fs::path(accountPaths(paths, id).filesDir) / "MARK" / "paper.pdf";
            fs::create_directories(file.parent_path());
            std::ofstream(file) << "a file of " << name;
        }
        REQUIRE(writeAccounts(paths.accountsFile(), st));

        SwitcherDeps d;
        d.paths = paths;
        d.device = &device;
        d.session.makeHttp = [] { return std::make_unique<FnHttp>([](const HttpRequest&) { return HttpResponse{}; }); };
        d.session.wifiUp = [this] { return online.load(); };
        d.session.wallMs = [this] { return wall.load(); };
        d.unlock = [this](int64_t id, std::function<void(bool)> done) { ui->unlock(id, std::move(done)); };
        app.setRoot(0, std::make_unique<SwitchingScreen>([] { return SwitchingScreen::View{"Zotero", "", false}; }, nullptr, nullptr));
        app.start();
        switcher = std::make_unique<AccountSwitcher>(app, d);
        switcher->onOpened([this](AccountSession& s) {
            s.startServices(app, nullptr, listening);
            app.replaceRoot(0, std::make_unique<AccountHome>(s.userId()));
            ui->opened(s);
        });
        AccountsUiDeps u;
        u.paths = paths;
        u.resetFile = (dir / "reset-pin.txt").string();
        u.wallMs = [this] { return wall.load(); };
        u.online = [this] { return online.load(); };
        u.checkKey = [](const Config& c, std::string* name) {
            const std::string owner = c.apiKey.rfind("key", 0) == 0 ? c.apiKey.substr(3) : "";
            if (owner != std::to_string(c.userId))
                return UserError{ErrorKind::ApiKeyRejected, "This API key belongs to another Zotero user (ID " + owner + ").", "userID mismatch"};
            if (name) *name = "user" + owner;
            return UserError{};
        };
        u.keyOwner = [this, check = u.checkKey](const Config& c) {
            if (ownerAnswer) return ownerAnswer(c);
            std::string name;
            return check(c, &name);
        };
        u.noAccount = [this](std::unique_ptr<Screen> s) {
            ++noAccounts;
            app.replaceRoot(0, std::move(s));
        };
        ui = std::make_unique<AccountsUi>(app, *switcher, u);
        app.setPollHook([this] { switcher->tick(); });
    }
    ~Rig() {
        app.setPollHook(nullptr);
        switcher->close();
        app.closeAll();
        ui.reset();
        switcher.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    AccountSession* session() const { return switcher->session(); }
    Screen* top() { return const_cast<Screen*>(app.top()); }
    template <class S> S* topAs() { return dynamic_cast<S*>(top()); }
    void tap(Rect r) {
        REQUIRE_FALSE(r.empty());
        top()->onGesture(tapOn(r), app);
        app.step(0);
    }
    bool settle() {
        const auto until = Clock::now() + std::chrono::seconds(20);
        while (Clock::now() < until) {
            app.step(0);
            if (!switcher->busy()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
    void openAt(int64_t id) {
        REQUIRE(switcher->switchTo(id, AccountSwitcher::Why::Startup));
        REQUIRE(settle());
    }
    void setPin(int64_t id, const std::string& pin, bool lockOnSleep = true) {
        REQUIRE(ui->pins().setPin(id, pin));
        REQUIRE(ui->pins().setLockOnSleep(id, lockOnSleep));
    }
    // Types the PIN on the pad that shows, then OK.
    void typePin(const std::string& pin) {
        auto* pad = topAs<PinPadScreen>();
        REQUIRE(pad);
        for (char ch : pin) pad->onGesture(tapOn(pad->keyRect(std::string(1, ch))), app);
        pad->onGesture(tapOn(pad->keyRect("OK")), app);
        app.step(0);
    }
    std::optional<AccountEntry> entry(int64_t id) { return ui->pins().entry(id); }
    int64_t homeOf() {
        auto* h = topAs<AccountHome>();
        return h ? h->userId : 0;
    }
};

// Settings → Accounts' data, as fixed values (the goldens).
struct FakeHost final : AccountsHost {
    std::vector<AccountSummary> list;
    std::vector<AccountSummary> accounts(int64_t) override { return list; }
    int64_t wallMs() override { return kWall; }
    void switchTo(int64_t) override {}
    void addAccount(Nav&) override {}
    void updateKey(Nav&, int64_t) override {}
    std::unique_ptr<Screen> pinPad(int64_t, PinPadScreen::Purpose, std::function<void(Nav&)>) override { return nullptr; }
    void setLockOnSleep(int64_t, bool) override {}
    bool online() override { return true; }
    uint64_t sendNow() override { return 1; }
    bool sent(uint64_t) override { return true; }
    void remove(Nav&, int64_t) override {}
};

AccountSummary pavel() {
    AccountSummary a;
    a.userId = 11;
    a.name = "Pavel";
    a.active = true;
    a.hasPin = true;
    a.lockOnSleep = true;
    a.items = 412;
    a.syncedWallMs = kWall - 10 * 60'000;
    a.bytes = 1'288'490'189;
    a.keyLine = "User ID 11 · files from Zotero storage";
    return a;
}

AccountSummary anna(bool locked = true) {
    AccountSummary a;
    a.userId = 22;
    a.name = "Anna";
    a.locked = locked;
    a.hasPin = locked;
    if (!locked) {
        a.items = 1280;
        a.unsent = 3;
        a.bytes = 1'288'490'189;
        a.keyLine = "User ID 22 · WebDAV: dav.example.org";
    }
    return a;
}

// What the KT4's screen shows of `c`: shrunk by ScaledDisplay onto 600×744.
void checkShrunk(const Canvas& c, const char* golden) {
    class Screen : public FakeDisplay {
    public:
        Canvas shown{600, 744};
        void present(const Canvas& from, Rect r, Waveform) override {
            cairo_t* cr = shown.cr();
            cairo_save(cr);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_clip(cr);
            cairo_set_source_surface(cr, from.surface(), 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    };
    auto s = std::make_unique<Screen>();
    s->w = 600;
    s->h = 744;
    Screen* screen = s.get();
    ScaledDisplay d(std::move(s), kKt4.scale);
    d.present(c, {0, 0, c.width(), c.height()}, Waveform::Full);
    checkGolden(screen->shown, golden);
}

// A PIN pad for Anna (PIN 1234) over an accounts.json of its own.
struct PadRig {
    fs::path dir = uniqueTempPath("zk-pin-pad");
    std::string file;
    int64_t wall = kWall, mono = 5'000;
    std::unique_ptr<AccountPins> pins;
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeNav nav{lib};
    PadRig() {
        fs::remove_all(dir);
        fs::create_directories(dir);
        file = (dir / "accounts.json").string();
        AccountsState st;
        st.active = 11;
        for (auto [id, name] : {std::pair<int64_t, const char*>{11, "Pavel"}, {22, "Anna"}}) {
            AccountEntry e;
            e.userId = id;
            e.name = name;
            e.order = static_cast<int>(st.accounts.size());
            st.accounts.push_back(e);
        }
        REQUIRE(writeAccounts(file, st));
        pins = std::make_unique<AccountPins>(file, [this] { return wall; }, [this] { return mono; });
        REQUIRE(pins->setPin(22, "1234"));
    }
    ~PadRig() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    PinPadScreen pad(PinPadScreen::Purpose p = PinPadScreen::Purpose::Unlock) {
        PinPadDeps d;
        d.userId = 22;
        d.name = "Anna";
        d.pins = pins.get();
        d.credentials = [] {
            Config c;
            c.userId = 22;
            c.apiKey = "key22";
            return std::optional<Config>(c);
        };
        d.resetFile = (dir / "reset-pin.txt").string();
        d.others = [] { return std::vector<AccountSheet::Row>{{11, "Pavel", false, false, "412 items · synced 10 min ago"}}; };
        d.useAnother = [](int64_t, Nav&) {};
        return PinPadScreen(p, std::move(d));
    }
    void type(PinPadScreen& pad, const std::string& pin) {
        for (char ch : pin) pad.onGesture(tapOn(pad.keyRect(std::string(1, ch))), nav);
        pad.onGesture(tapOn(pad.keyRect("OK")), nav);
    }
};

}  // namespace

// ---- the chip, the sheet, Settings → Accounts -----------------------------------------------------

TEST_CASE("account screens: one account — no chip on Home; Settings shows Accounts with one row") {
    Rig k({{11, "Pavel"}});
    k.openAt(11);
    CHECK(k.ui->chip().empty());
    SyncStatus status;
    HomeActions actions;
    actions.accountChip = [&] { return k.ui->chip(); };
    HomeScreen home(status, actions, [] { return kWall; });
    home.onShow(k.app);
    CHECK(home.chipRect().empty());
    AccountsScreen list(*k.ui);
    list.onShow(k.app);
    REQUIRE(list.shown().size() == 1);
    CHECK(list.shown()[0].name == "Pavel");
    CHECK(list.shown()[0].active);
    CHECK_FALSE(list.addRect().empty());  // room for two more
}

TEST_CASE("account screens: two accounts — Home's chip names the open one and opens the sheet") {
    Rig k;
    k.openAt(11);
    CHECK(k.ui->chip() == "Pavel");
    SyncStatus status;
    HomeActions actions;
    actions.accountChip = [&] { return k.ui->chip(); };
    actions.accountSheet = [&] { return k.ui->sheet(); };
    HomeScreen home(status, actions, [] { return kWall; });
    home.onShow(k.app);
    REQUIRE_FALSE(home.chipRect().empty());
    home.onGesture(tapOn(home.chipRect()), k.app);
    REQUIRE(home.accountSheet());
    const auto& rows = home.accountSheet()->rows();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].name == "Pavel");
    CHECK(rows[0].active);
    CHECK(rows[1].name == "Anna");
    CHECK_FALSE(home.accountSheet()->addRect({0, 0, 1236, 1417}).empty());
}

TEST_CASE("account screens: the sheet shows a locked account's name and lock only; + Add account goes at three") {
    Rig k({{11, "Pavel"}, {22, "Anna"}, {33, "Lena"}});
    k.setPin(22, "1234");
    k.openAt(11);
    // Anna's session kept what it had (items, something unsent): still nothing of it shows.
    REQUIRE(keepAccountSummary(k.paths.accountsFile(), 22, 77, 2, kWall - 60'000));
    const AccountSheet sheet = k.ui->sheet();
    REQUIRE(sheet.rows().size() == 3);
    const auto& a = sheet.rows()[1];
    CHECK(a.name == "Anna");
    CHECK(a.locked);
    CHECK(a.line.empty());
    for (const auto& s : k.ui->accounts())
        if (s.userId == 22) {
            CHECK(s.items == 0);
            CHECK(s.unsent == 0);
        }
    CHECK_FALSE(sheet.rows()[2].locked);
    CHECK(sheet.rows()[2].line == "0 items");
    CHECK(sheet.addRect({0, 0, 1236, 1417}).empty());  // three: no more
}

// ---- Add account, Update key ---------------------------------------------------------------------

TEST_CASE("account screens: Add account refuses a user already on the Kindle, before the network") {
    Rig k;
    k.openAt(11);
    k.ui->addAccount(k.app);
    auto* setup = k.topAs<SetupScreen>();
    REQUIRE(setup);
    CHECK(setup->purpose().kind == SetupPurpose::Kind::Add);
    setup->onGesture(tapOn(SetupScreen::fieldRect(0)), k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Text, "22"}, k.app);
    setup->onGesture(tapOn(SetupScreen::fieldRect(1)), k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Text, "key22"}, k.app);
    setup->onGesture(tapOn(SetupScreen::connectRect()), k.app);
    REQUIRE(setup->error());
    CHECK(setup->error()->message == "Anna is already on this Kindle");
    CHECK_FALSE(setup->checking());
}

TEST_CASE("account screens: Add account with a new user's key adds it, switches to it, and nothing of it is made before the check") {
    Rig k;
    k.openAt(11);
    k.ui->addAccount(k.app);
    auto* setup = k.topAs<SetupScreen>();
    REQUIRE(setup);
    setup->onGesture(tapOn(SetupScreen::fieldRect(0)), k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Text, "33"}, k.app);
    setup->onGesture(tapOn(SetupScreen::fieldRect(1)), k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Text, "key33"}, k.app);
    setup->onGesture(tapOn(SetupScreen::connectRect()), k.app);
    CHECK_FALSE(fs::exists(accountPaths(k.paths, 33).root));
    CHECK_FALSE(loadCredentials(k.paths, 33));
    REQUIRE(eventually([&] {
        k.app.step(0);
        return k.session() && k.session()->userId() == 33 && !k.switcher->busy();
    }, 10000));
    REQUIRE(k.entry(33));
    CHECK(k.entry(33)->name == "user33");
    CHECK(loadCredentials(k.paths, 33)->apiKey == "key33");
    CHECK_FALSE(fs::exists(k.paths.configFile()));  // Add account writes no config.json
}

TEST_CASE("account screens: a key dropped over USB for an account is kept only once Zotero says it is that account's") {
    Rig k;
    k.openAt(11);
    auto drop = [&](const std::string& text) { std::ofstream(k.paths.droppedConfig) << text; };
    auto answered = [&](const std::string& toast) {
        return eventually([&] {
            k.app.step(0);
            k.ui->poll();
            return k.app.lastToast() == toast;
        }, 10000);
    };
    // Someone else's key, said to be Anna's: offline, nothing changes and it waits.
    drop(R"({"userId":22,"apiKey":"key99"})");
    k.ui->checkDroppedKey();
    CHECK(k.app.lastToast() == "The key for Anna will be checked when Wi-Fi is on");
    CHECK(loadCredentials(k.paths, 22)->apiKey == "key22");
    CHECK(fs::exists(k.paths.droppedConfig));
    // Online: Zotero says it isn't Anna's — refused, the file left, Anna's key as it was.
    k.online = true;
    k.ui->checkDroppedKey();
    REQUIRE(answered("The key in config.json isn't Anna's, so it wasn't used"));
    CHECK(loadCredentials(k.paths, 22)->apiKey == "key22");
    CHECK(fs::exists(k.paths.droppedConfig));
    // The open account's own key, with new WebDAV details: kept, the file gone.
    drop(R"({"userId":11,"apiKey":"key11","webdavUrl":"https://dav.new/"})");
    k.ui->checkDroppedKey();
    REQUIRE(answered("The new key of Pavel from config.json is kept"));
    CHECK(loadCredentials(k.paths, 11)->webdavUrl == "https://dav.new/");
    CHECK(loadCredentials(k.paths, 22)->apiKey == "key22");
    CHECK_FALSE(fs::exists(k.paths.droppedConfig));
}

TEST_CASE("account screens: a first account added from a dropped config.json leaves no config.json behind") {
    Rig k({{11, "Pavel"}});
    Config dropped;
    dropped.userId = 33;
    dropped.apiKey = "key33";
    REQUIRE(saveConfig(k.paths.configDir, dropped));  // kept by startup for Setup (startup.h, FirstAccount)
    k.app.push(k.ui->firstAccountSetup(dropped, true));
    auto* setup = k.topAs<SetupScreen>();
    REQUIRE(setup);
    CHECK(setup->config().userId == 33);
    setup->onGesture(tapOn(SetupScreen::connectRect()), k.app);
    REQUIRE(eventually([&] {
        k.app.step(0);
        return k.session() && k.session()->userId() == 33 && !k.switcher->busy();
    }, 10000));
    CHECK(loadCredentials(k.paths, 33)->apiKey == "key33");
    CHECK_FALSE(fs::exists(k.paths.configFile()));  // no second copy of the credentials
}

TEST_CASE("account screens: Update key refuses another user's key; the same user's new key is kept") {
    Rig k;
    k.openAt(11);
    k.ui->updateKey(k.app, 11);
    auto* setup = k.topAs<SetupScreen>();
    REQUIRE(setup);
    CHECK(setup->purpose().kind == SetupPurpose::Kind::Update);
    CHECK(setup->config().userId == 11);
    // Another user's ID: refused before the network.
    setup->onGesture(tapOn(SetupScreen::fieldRect(0)), k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Backspace}, k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Backspace}, k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Text, "22"}, k.app);
    setup->onGesture(tapOn(SetupScreen::connectRect()), k.app);
    REQUIRE(setup->error());
    CHECK(setup->error()->message == "This key is for another account — use Add account");
    // This user's ID with another user's key: Zotero says whose it is.
    setup->onGesture(tapOn(SetupScreen::fieldRect(0)), k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Backspace}, k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Backspace}, k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Text, "11"}, k.app);
    setup->onGesture(tapOn(SetupScreen::fieldRect(1)), k.app);
    for (int i = 0; i < 5; ++i) setup->onKey(KeyEvent{KeyEvent::Code::Backspace}, k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Text, "key22"}, k.app);
    setup->onGesture(tapOn(SetupScreen::connectRect()), k.app);
    REQUIRE(eventually([&] {
        k.app.step(0);
        return !setup->checking();
    }));
    REQUIRE(setup->error());
    CHECK(setup->error()->message == "This key is for another account — use Add account");
    CHECK(loadCredentials(k.paths, 11)->apiKey == "key11");
    // The same user's key (as a new one would be): kept.
    setup->onGesture(tapOn(SetupScreen::fieldRect(1)), k.app);
    for (int i = 0; i < 5; ++i) setup->onKey(KeyEvent{KeyEvent::Code::Backspace}, k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Text, "key11"}, k.app);
    setup->onGesture(tapOn(SetupScreen::fieldRect(2)), k.app);
    setup->onKey(KeyEvent{KeyEvent::Code::Text, "https://dav.example.org/"}, k.app);
    setup->onGesture(tapOn(SetupScreen::connectRect()), k.app);
    REQUIRE(eventually([&] {
        k.app.step(0);
        return !k.topAs<SetupScreen>();
    }));
    CHECK(loadCredentials(k.paths, 11)->webdavUrl == "https://dav.example.org/");
    CHECK(loadCredentials(k.paths, 22)->apiKey == "key22");
}

// ---- the PIN pad ---------------------------------------------------------------------------------

TEST_CASE("account screens: a PIN entered twice differently is refused; the same twice is kept with Lock on sleep on") {
    PadRig r;
    REQUIRE(r.pins->removePin(22));
    bool done = false;
    PinPadDeps d;
    d.userId = 22;
    d.name = "Anna";
    d.pins = r.pins.get();
    d.done = [&](Nav&) { done = true; };
    PinPadScreen pad(PinPadScreen::Purpose::Set, d);
    pad.onShow(r.nav);
    CHECK(pad.stage() == PinPadScreen::Stage::Choose);
    r.type(pad, "1234");
    CHECK(pad.stage() == PinPadScreen::Stage::Confirm);
    r.type(pad, "5678");
    CHECK(pad.stage() == PinPadScreen::Stage::Choose);
    CHECK(pad.message() == "The PINs didn't match. Choose a PIN again.");
    CHECK_FALSE(done);
    CHECK_FALSE(r.pins->entry(22)->pin);
    r.type(pad, "123");  // too short
    CHECK(pad.message() == "A PIN is 4 to 6 digits.");
    for (int i = 0; i < 3; ++i) pad.onGesture(tapOn(pad.keyRect("⌫")), r.nav);
    r.type(pad, "246810");
    r.type(pad, "246810");
    CHECK(done);
    const auto e = r.pins->entry(22);
    REQUIRE(e->pin);
    CHECK(checkPin(*e->pin, "246810"));
    CHECK(e->lockOnSleep);
}

TEST_CASE("account screens: a lock with no way back can still close Zotero; with Back, the close button isn't there") {
    PadRig r;
    PinPadScreen lock = r.pad();  // no back: the lock on wake, or the only account at startup
    lock.onShow(r.nav);
    CHECK_FALSE(lock.canGoBack());
    lock.onGesture(tapOn(lock.closeRect()), r.nav);
    CHECK(r.nav.quitted);
    CHECK(r.pins->entry(22)->pin);  // closing passes no lock

    PadRig r2;
    bool back = false;
    PinPadDeps d;
    d.userId = 22;
    d.name = "Anna";
    d.pins = r2.pins.get();
    d.back = [&](Nav&) { back = true; };
    PinPadScreen withBack(PinPadScreen::Purpose::Unlock, d);
    withBack.onShow(r2.nav);
    CHECK(withBack.canGoBack());
    withBack.onGesture(tapOn(withBack.closeRect()), r2.nav);
    CHECK_FALSE(r2.nav.quitted);
}

TEST_CASE("account screens: the sixth wrong PIN waits 30 s, kept in accounts.json; the keys wait too; then the right PIN opens") {
    PadRig r;
    PinPadScreen pad = r.pad();
    pad.onShow(r.nav);
    for (int i = 0; i < 5; ++i) {
        r.type(pad, "0000");
        CHECK(pad.message() == "Wrong PIN.");
        CHECK_FALSE(pad.waiting());
    }
    r.type(pad, "0000");
    CHECK(pad.waiting());
    CHECK(pad.message() == "Wrong PIN. Try again in 30 s");
    CHECK(readAccounts(r.file)->accounts[1].backoff.misses == 6);
    // The right PIN while waiting: the keys don't take it.
    r.type(pad, "1234");
    CHECK(pad.entry().empty());
    CHECK(pad.waiting());
    // The wall clock moved back: the wait still holds within this run.
    r.wall -= 3'600'000;
    r.mono += 10'000;
    pad.onTick(r.nav);
    CHECK(pad.waiting());
    CHECK(pad.message() == "Wrong PIN. Try again in 20 s");
    r.wall += 3'600'000 + 31'000;
    r.mono += 21'000;
    pad.onTick(r.nav);
    CHECK_FALSE(pad.waiting());
    bool opened = false;
    PinPadDeps d;
    d.userId = 22;
    d.name = "Anna";
    d.pins = r.pins.get();
    d.done = [&](Nav&) { opened = true; };
    PinPadScreen again(PinPadScreen::Purpose::Unlock, d);
    again.onShow(r.nav);
    r.type(again, "1234");
    CHECK(opened);
    CHECK(readAccounts(r.file)->accounts[1].backoff.misses == 0);
}

TEST_CASE("account screens: Forgot PIN? — reset-pin.txt with the account's key offers Set a new PIN; the file is gone") {
    PadRig r;
    PinPadScreen pad = r.pad();
    pad.onShow(r.nav);
    pad.onGesture(tapOn(pad.forgotRect()), r.nav);
    REQUIRE(pad.stage() == PinPadScreen::Stage::Forgot);
    CHECK(pad.prompt().find("reset-pin.txt") != std::string::npos);
    CHECK(pad.prompt().find("zotero folder") != std::string::npos);
    // None there yet: nothing counted.
    pad.onGesture(tapOn(pad.checkFileRect()), r.nav);
    CHECK(pad.message() == "There's no reset-pin.txt in the zotero folder.");
    CHECK(readAccounts(r.file)->accounts[1].backoff.misses == 0);
    std::ofstream(r.dir / "reset-pin.txt") << "  key22\n";
    pad.onGesture(tapOn(pad.checkFileRect()), r.nav);
    CHECK(pad.stage() == PinPadScreen::Stage::Reset);
    CHECK_FALSE(fs::exists(r.dir / "reset-pin.txt"));
    CHECK_FALSE(pad.newPinRect().empty());
    pad.onGesture(tapOn(pad.newPinRect()), r.nav);
    CHECK(pad.stage() == PinPadScreen::Stage::Choose);
}

TEST_CASE("account screens: Forgot PIN? — another account's key is refused, the file is gone, and it counts as a wrong try") {
    PadRig r;
    PinPadScreen pad = r.pad();
    pad.onShow(r.nav);
    pad.onGesture(tapOn(pad.forgotRect()), r.nav);
    std::ofstream(r.dir / "reset-pin.txt") << "key11";
    pad.onGesture(tapOn(pad.checkFileRect()), r.nav);
    CHECK(pad.stage() == PinPadScreen::Stage::Forgot);
    CHECK(pad.message() == "This key isn't this account's");
    CHECK_FALSE(fs::exists(r.dir / "reset-pin.txt"));
    CHECK(readAccounts(r.file)->accounts[1].backoff.misses == 1);
    // Typed, offline: the same.
    pad.onGesture(tapOn(pad.typeKeyRect()), r.nav);
    REQUIRE(pad.stage() == PinPadScreen::Stage::TypeKey);
    pad.onKey(KeyEvent{KeyEvent::Code::Text, "nobody's"}, r.nav);
    pad.onGesture(tapOn(pad.checkKeyRect()), r.nav);
    CHECK(pad.message() == "This key isn't this account's");
    CHECK(readAccounts(r.file)->accounts[1].backoff.misses == 2);
}

TEST_CASE("account screens: a typed key that isn't the one on the Kindle counts as a wrong try, offline, without an answer, or refused") {
    PadRig r;
    bool online = false;
    UserError answer{ErrorKind::HostNotFound, "No connection to Zotero.", "curl 6"};
    std::atomic<int> asked{0};
    PinPadDeps d;
    d.userId = 22;
    d.name = "Anna";
    d.pins = r.pins.get();
    d.credentials = [] {
        Config c;
        c.userId = 22;
        c.apiKey = "key22";
        return std::optional<Config>(c);
    };
    d.checkKey = [&](const Config&) {
        ++asked;
        return answer;
    };
    d.online = [&] { return online; };
    std::optional<Config> kept;
    d.keepKey = [&](const Config& c) { kept = c; };
    PinPadScreen pad(PinPadScreen::Purpose::Unlock, d);
    pad.onShow(r.nav);
    pad.onGesture(tapOn(pad.forgotRect()), r.nav);
    pad.onGesture(tapOn(pad.typeKeyRect()), r.nav);
    auto typeKey = [&](const std::string& key) {
        for (int i = 0; i < 40; ++i) pad.onKey(KeyEvent{KeyEvent::Code::Backspace}, r.nav);
        pad.onKey(KeyEvent{KeyEvent::Code::Text, key}, r.nav);
        pad.onGesture(tapOn(pad.checkKeyRect()), r.nav);
        REQUIRE(eventually([&] {
            pad.onTick(r.nav);
            return pad.message().rfind("Not the key on this Kindle", 0) != 0;
        }));
    };
    auto misses = [&] { return readAccounts(r.file)->accounts[1].backoff.misses; };
    typeKey("wrong1");  // offline: not even asked
    CHECK(asked == 0);
    CHECK(misses() == 1);
    CHECK(pad.message() == "This key isn't this account's");
    online = true;
    typeKey("wrong2");  // asked, no answer
    CHECK(asked == 1);
    CHECK(misses() == 2);
    CHECK(pad.message() == "This key isn't this account's");
    answer = {ErrorKind::ApiKeyRejected, "This API key belongs to another Zotero user.", "userID mismatch"};
    typeKey("wrong3");  // refused
    CHECK(misses() == 3);
    CHECK_FALSE(kept);
    // A new key Zotero says is Anna's: kept, the wrong tries forgiven, Set a new PIN / Remove PIN.
    answer = {};
    typeKey("newkey22");
    CHECK(pad.stage() == PinPadScreen::Stage::Reset);
    REQUIRE(kept);
    CHECK(kept->apiKey == "newkey22");
    CHECK(kept->userId == 22);
    CHECK(misses() == 0);
}

TEST_CASE("account screens: reset-pin.txt is deleted even while a wait runs, and counts nothing then") {
    PadRig r;
    PinPadScreen pad = r.pad();
    pad.onShow(r.nav);
    for (int i = 0; i < 6; ++i) r.type(pad, "0000");
    REQUIRE(pad.waiting());
    // (Forgot PIN? from a pad shown afresh, the wait still running)
    PinPadScreen forgot = r.pad();
    forgot.onShow(r.nav);
    forgot.onGesture(tapOn(forgot.forgotRect()), r.nav);
    std::ofstream(r.dir / "reset-pin.txt") << "key22";
    forgot.onGesture(tapOn(forgot.checkFileRect()), r.nav);
    CHECK_FALSE(fs::exists(r.dir / "reset-pin.txt"));
    CHECK(forgot.stage() == PinPadScreen::Stage::Forgot);
    CHECK(forgot.message().rfind("Wrong tries: put reset-pin.txt back and try again in ", 0) == 0);
    CHECK(readAccounts(r.file)->accounts[1].backoff.misses == 6);
}

// ---- the lock --------------------------------------------------------------------------------------

TEST_CASE("account screens: switching to a locked account asks its PIN over the open one; Back leaves it as it was; the right PIN switches") {
    Rig k;
    k.setPin(22, "2468");
    k.openAt(11);
    k.app.push(std::make_unique<AccountsScreen>(*k.ui));
    Screen* list = k.top();
    k.ui->switchTo(22);
    k.app.step(0);
    auto* pad = k.topAs<PinPadScreen>();
    REQUIRE(pad);
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 11);
    k.tap(headerBackRect());
    CHECK_FALSE(k.app.locked());
    CHECK(k.top() == list);  // nothing of Pavel was left
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 11);
    CHECK_FALSE(k.switcher->busy());
    k.ui->switchTo(22);
    k.app.step(0);
    k.typePin("2468");
    REQUIRE(k.settle());
    k.app.step(0);
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 22);
    CHECK(k.homeOf() == 22);
    CHECK_FALSE(k.app.locked());
}

TEST_CASE("account screens: the sheet shows an account that isn't open from the account list, never from its library") {
    Rig k;
    k.openAt(11);
    fillFixtureLibrary(k.session()->library(), 6);
    k.session()->library().markRead(fixtureKey("IT", 1), true);
    const int64_t items = k.session()->itemCount();
    REQUIRE(items > 0);
    // A good sync: the summary kept, with when.
    SyncEvent done;
    done.kind = SyncEvent::Kind::SyncFinished;
    k.ui->onSyncEvent(done);
    CHECK(k.entry(11)->itemCount == items);
    CHECK(k.entry(11)->unsentCount == 1);
    CHECK(k.entry(11)->syncedWallMs == kWall);
    // Anna's library is never opened for the sheet (it doesn't even exist yet).
    const AccountSheet sheet = k.ui->sheet();
    CHECK(sheet.rows()[1].line == "0 items");
    CHECK_FALSE(fs::exists(accountPaths(k.paths, 22).dbPath));
    // Pavel left: what his session kept when it stopped is what the sheet shows of him.
    k.session()->library().markRead(fixtureKey("IT", 2), true);
    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::User));
    REQUIRE(k.settle());
    CHECK(k.entry(11)->unsentCount == 2);
    for (const auto& row : k.ui->sheet().rows())
        if (row.userId == 11) CHECK(row.line == thousands(items) + " items · 2 changes not sent");
}

TEST_CASE("account screens: Settings → Accounts' Manage… opens the list") {
    Rig k({{11, "Pavel"}});
    k.openAt(11);
    Config c;
    c.userId = 11;
    k.app.push(std::make_unique<SettingsScreen>(c, [&](Nav& nav) { nav.push(std::make_unique<AccountsScreen>(*k.ui)); }));
    k.tap(k.topAs<SettingsScreen>()->accountRect());
    auto* list = k.topAs<AccountsScreen>();
    REQUIRE(list);
    CHECK(list->shown().size() == 1);
}

TEST_CASE("account screens: wake with Lock on sleep — the PIN pad covers the reader, which keeps its page once unlocked") {
    Rig k;
    k.setPin(11, "1357");
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));  // asked at startup: the PIN pad first
    k.app.step(0);
    REQUIRE(k.topAs<PinPadScreen>());
    CHECK_FALSE(k.session());
    k.typePin("1357");
    REQUIRE(k.settle());
    REQUIRE(k.session());
    fillFixtureLibrary(k.session()->library(), 12);
    std::optional<ItemRow> row;
    for (const auto& r : k.app.library().rows(ListQuery{}))
        if (!r.attachmentKey.empty() && !row) row = r;
    REQUIRE(row);
    auto reader = std::make_unique<ReaderScreen>(*row, "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    ReaderScreen* r = reader.get();
    k.app.push(std::move(reader));
    REQUIRE(eventually([&] {
        k.app.step(0);
        return r->ready();
    }));
    r->turnTo(3);
    REQUIRE(eventually([&] {
        k.app.step(0);
        return r->page() == 3 && r->ready();
    }));
    SyncStatus status;
    Lifecycle lifecycle(k.nullSync, status, [] { return 10; }, [] { return true; });
    lifecycle.attach(k.app);
    bool asleep = false;
    lifecycle.setLock([&] { k.ui->lockNow(); }, [&] { return asleep; });
    // A system dialog or the keyboard over the app (FocusOut): no lock.
    InputEvent dialog;
    dialog.kind = InputEvent::Kind::Background;
    k.display.input.push_back(dialog);
    k.app.step(0);
    CHECK_FALSE(k.app.locked());
    InputEvent back;
    back.kind = InputEvent::Kind::Foreground;
    k.display.input.push_back(back);
    k.app.step(0);
    CHECK(k.top() == r);
    asleep = true;  // the screensaver
    InputEvent sleep;
    sleep.kind = InputEvent::Kind::Background;
    k.display.input.push_back(sleep);
    k.app.step(0);
    REQUIRE(k.app.locked());
    auto* pad = k.topAs<PinPadScreen>();
    REQUIRE(pad);
    CHECK(pad->prompt() == "Enter the PIN for Pavel");
    InputEvent wake;
    wake.kind = InputEvent::Kind::Foreground;
    k.display.input.push_back(wake);
    k.app.step(0);
    CHECK(k.topAs<PinPadScreen>());  // still over it
    CHECK(k.session());              // the session went on under the pad
    k.typePin("1357");
    CHECK_FALSE(k.app.locked());
    CHECK(k.top() == r);
    REQUIRE(eventually([&] {
        k.app.step(0);
        return r->ready();
    }));
    CHECK(r->page() == 3);
    RenderWorker::waitForAll(3000);
}

namespace {

// Pavel (11, PIN 1357, Lock on sleep) open and then locked on sleep; Anna (22) has PIN 2468.
void lockedPavel(Rig& k) {
    k.setPin(11, "1357");
    k.setPin(22, "2468");
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.app.step(0);
    k.typePin("1357");
    REQUIRE(k.settle());
    REQUIRE(k.homeOf() == 11);
    k.ui->lockNow();
    k.app.step(0);
    REQUIRE(k.app.locked());
}

// The pad that shows asks for `who`'s PIN.
bool padFor(Rig& k, const std::string& who) {
    auto* pad = k.topAs<PinPadScreen>();
    return pad && k.app.locked() && pad->prompt() == "Enter the PIN for " + who;
}

// The locked pad's Use another account, then (a chooser: more than one, or a locked one) the row of `userId`.
void useAnother(Rig& k, int64_t userId) {
    auto* pad = k.topAs<PinPadScreen>();
    REQUIRE(pad);
    k.tap(pad->anotherRect());
    if (auto* p = k.topAs<PinPadScreen>(); p && p == pad && p->chooser()) {
        const Rect area{0, 0, k.display.w, k.display.h};
        const auto& rows = p->chooser()->rows();
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].userId == userId) k.tap(p->chooser()->rowRect(area, i));
    }
    k.app.step(0);
}

}  // namespace

TEST_CASE("account screens: locked on sleep, Use another account → a locked one → Back ends on the open account's own pad, never its library") {
    Rig k;
    lockedPavel(k);
    REQUIRE(padFor(k, "Pavel"));
    useAnother(k, 22);
    REQUIRE(padFor(k, "Anna"));
    CHECK(k.session()->userId() == 11);  // still open, under the pads
    k.tap(headerBackRect());
    k.app.step(0);
    CHECK(padFor(k, "Pavel"));
    CHECK_FALSE(k.switcher->busy());
    CHECK_FALSE(k.topAs<AccountHome>());
}

TEST_CASE("account screens: locked on sleep, another account's pad → Use another account → the open one shows the open one's pad") {
    Rig k;
    lockedPavel(k);
    useAnother(k, 22);
    REQUIRE(padFor(k, "Anna"));
    useAnother(k, 11);  // Anna's only other: Pavel, open (no chooser)
    CHECK(padFor(k, "Pavel"));
    CHECK(k.session()->userId() == 11);
    // Pavel's right PIN: as before.
    k.typePin("1357");
    k.app.step(0);
    CHECK_FALSE(k.app.locked());
    CHECK(k.homeOf() == 11);
}

TEST_CASE("account screens: sleep while another account's PIN is asked over the open one locks the open one; its PIN brings everything back") {
    Rig k;
    k.setPin(11, "1357");
    k.setPin(22, "2468");
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.app.step(0);
    k.typePin("1357");
    REQUIRE(k.settle());
    k.ui->switchTo(22);
    k.app.step(0);
    REQUIRE(padFor(k, "Anna"));
    REQUIRE(k.switcher->asking());
    k.ui->lockNow();  // the Kindle sleeps
    k.app.step(0);
    CHECK(padFor(k, "Pavel"));
    CHECK_FALSE(k.switcher->busy());  // Anna's question dropped
    CHECK(k.topAs<PinPadScreen>()->anotherRect().w > 0);
    k.typePin("0000");  // wrong: still locked
    CHECK(padFor(k, "Pavel"));
    k.typePin("1357");
    k.app.step(0);
    CHECK_FALSE(k.app.locked());
    CHECK(k.homeOf() == 11);
    // And switching works as before.
    k.ui->switchTo(22);
    k.app.step(0);
    k.typePin("2468");
    REQUIRE(k.settle());
    k.app.step(0);
    CHECK(k.homeOf() == 22);
}

TEST_CASE("account screens: a suspend not seen as the app went to the background still locks on coming back") {
    Rig k;
    k.setPin(11, "1357");
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.app.step(0);
    k.typePin("1357");
    REQUIRE(k.settle());
    SyncStatus status;
    Lifecycle lifecycle(k.nullSync, status, [] { return 10; }, [] { return true; });
    lifecycle.attach(k.app);
    int64_t suspended = 5'000;
    lifecycle.setLock([&] { k.ui->lockNow(); }, [] { return false; }, [&] { return suspended; });
    InputEvent away;
    away.kind = InputEvent::Kind::Background;
    k.display.input.push_back(away);
    k.app.step(0);
    CHECK_FALSE(k.app.locked());
    suspended += 600'000;  // the Kindle slept ten minutes meanwhile
    InputEvent back;
    back.kind = InputEvent::Kind::Foreground;
    k.display.input.push_back(back);
    k.app.step(0);
    CHECK(k.app.locked());
    CHECK(k.topAs<PinPadScreen>());
}

TEST_CASE("account screens: Lock on sleep off, or no PIN — waking shows what was there") {
    Rig k;
    k.setPin(11, "1357", false);
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.app.step(0);
    k.typePin("1357");
    REQUIRE(k.settle());
    k.ui->lockNow();
    CHECK_FALSE(k.app.locked());
    CHECK(k.homeOf() == 11);
}

TEST_CASE("account screens: at startup a locked account opens nothing until unlocked; Use another account opens the other without it") {
    Rig k;
    k.setPin(11, "1357");
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.app.step(0);
    auto* pad = k.topAs<PinPadScreen>();
    REQUIRE(pad);
    CHECK_FALSE(k.session());
    CHECK_FALSE(fs::exists(accountPaths(k.paths, 11).dbPath));  // nothing of it opened
    REQUIRE_FALSE(pad->anotherRect().empty());
    k.tap(pad->anotherRect());  // one other, unlocked: straight to it
    REQUIRE(k.settle());
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 22);
    CHECK(k.homeOf() == 22);
    CHECK_FALSE(k.app.locked());
    CHECK_FALSE(fs::exists(accountPaths(k.paths, 11).dbPath));
    CHECK(readAccounts(k.paths.accountsFile())->active == 22);
}

// ---- Remove from this Kindle -------------------------------------------------------------------------

TEST_CASE("account screens: removing the open account with unsent changes offers Send now; then it switches away and deletes only that account") {
    Rig k;
    k.openAt(11);
    k.online = true;
    fillFixtureLibrary(k.session()->library(), 6);
    k.session()->library().markRead(fixtureKey("IT", 1), true);
    k.session()->library().markRead(fixtureKey("IT", 2), true);
    k.app.push(std::make_unique<AccountsScreen>(*k.ui));
    k.app.push(std::make_unique<AccountPageScreen>(*k.ui, 11));
    k.tap(k.topAs<AccountPageScreen>()->removeRect());
    auto* remove = k.topAs<RemoveAccountScreen>();
    REQUIRE(remove);
    CHECK(remove->shown().unsent == 2);
    CHECK(remove->unsentLine().find("2 changes made on this Kindle haven't been sent") == 0);
    CHECK_FALSE(remove->sendRect().empty());
    CHECK(remove->removeLabel().rfind("Remove Pavel and delete ", 0) == 0);
    const std::string annaFile = slurpFile(accountPaths(k.paths, 22).filesDir + "/MARK/paper.pdf");
    k.tap(remove->removeRect());
    REQUIRE(k.settle());
    k.app.step(0);
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 22);
    CHECK(k.homeOf() == 22);
    CHECK_FALSE(fs::exists(accountPaths(k.paths, 11).root));
    CHECK_FALSE(fs::exists(k.paths.credentialsFile(11)));
    CHECK(slurpFile(accountPaths(k.paths, 22).filesDir + "/MARK/paper.pdf") == annaFile);
    CHECK(loadCredentials(k.paths, 22));
    const auto st = readAccounts(k.paths.accountsFile());
    REQUIRE(st->accounts.size() == 1);
    CHECK(st->accounts[0].userId == 22);
    CHECK(st->active == 22);
}

TEST_CASE("account screens: removing an account that isn't open deletes it at once; the open one stays") {
    Rig k;
    k.openAt(11);
    k.app.push(std::make_unique<AccountsScreen>(*k.ui));
    k.app.push(std::make_unique<AccountPageScreen>(*k.ui, 22));
    k.tap(k.topAs<AccountPageScreen>()->removeRect());
    k.tap(k.topAs<RemoveAccountScreen>()->removeRect());
    REQUIRE(k.topAs<AccountsScreen>());
    CHECK(k.topAs<AccountsScreen>()->shown().size() == 1);
    CHECK_FALSE(fs::exists(accountPaths(k.paths, 22).root));
    CHECK(fs::exists(accountPaths(k.paths, 11).filesDir + "/MARK/paper.pdf"));
    CHECK(k.session()->userId() == 11);
}

TEST_CASE("account screens: a locked account can be removed without its PIN, behind the warning") {
    Rig k;
    k.setPin(22, "2468");
    k.openAt(11);
    k.app.push(std::make_unique<RemoveAccountScreen>(*k.ui, 22));
    auto* remove = k.topAs<RemoveAccountScreen>();
    CHECK(remove->shown().locked);
    CHECK(remove->unsentLine().empty());
    k.tap(remove->removeRect());
    CHECK_FALSE(k.topAs<PinPadScreen>());
    CHECK_FALSE(fs::exists(accountPaths(k.paths, 22).root));
}

TEST_CASE("account screens: removing the last account leaves every screen (a reader too) before its session stops, and leaves Setup for a first account") {
    Rig k({{11, "Pavel"}});
    k.openAt(11);
    fillFixtureLibrary(k.session()->library(), 12);
    std::optional<ItemRow> row;
    for (const auto& r : k.app.library().rows(ListQuery{}))
        if (!r.attachmentKey.empty() && !row) row = r;
    REQUIRE(row);
    auto reader = std::make_unique<ReaderScreen>(*row, "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    ReaderScreen* r = reader.get();
    k.app.push(std::move(reader));
    REQUIRE(eventually([&] {
        k.app.step(0);
        return r->ready();
    }));
    k.app.push(std::make_unique<RemoveAccountScreen>(*k.ui, 11));
    k.top()->onGesture(tapOn(k.topAs<RemoveAccountScreen>()->removeRect()), k.app);
    CHECK(k.session());  // the screens go first; the session a step later
    CHECK(k.switcher->busy());
    REQUIRE(k.settle());
    k.app.step(0);
    CHECK(k.noAccounts == 1);
    CHECK_FALSE(k.session());
    auto* setup = k.topAs<SetupScreen>();
    REQUIRE(setup);
    CHECK(setup->purpose().kind == SetupPurpose::Kind::First);  // "Set up Zotero", as on an empty Kindle
    CHECK_FALSE(setup->purpose().keepsConfigFile);
    CHECK(setup->config().userId == 0);  // empty, not the removed account's key
    CHECK_FALSE(fs::exists(accountPaths(k.paths, 11).root));
    CHECK_FALSE(fs::exists(k.paths.credentialsFile(11)));
    CHECK(readAccounts(k.paths.accountsFile())->accounts.empty());
}

// ---- goldens -------------------------------------------------------------------------------------------

TEST_CASE("account screens: goldens — the list, a page, the sheet, the chip, the PIN pad, its wait, Forgot PIN?, Remove") {
    if (!fonts()) return;
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 40);
    lib.markOpened("IT000006", "EP000006", kWall - 26 * 3600 * 1000LL);
    FakeNav nav(lib);
    Canvas c(1236, 1547);
    FakeHost host;
    host.list = {pavel(), anna()};

    AccountsScreen list(host);
    list.onShow(nav);
    list.draw(c);
    checkGolden(c, "tests/golden/accounts-list.png");

    AccountPageScreen page(host, 11);
    page.onShow(nav);
    page.draw(c);
    checkGolden(c, "tests/golden/account-page.png");

    SyncStatus status;
    HomeActions actions;
    actions.accountChip = [] { return std::string("Pavel"); };
    actions.accountSheet = [] {
        return AccountSheet({{11, "Pavel", true, false, accountLine(pavel(), kWall)}, {22, "Anna", false, true, ""}}, true, true);
    };
    HomeScreen home(status, actions, [] { return kWall; });
    auto withTabs = [&](Screen& s, const char* golden) {
        s.draw(c);
        drawTabBar(c.cr(), {0, 1547 - kTabBarH, 1236, kTabBarH}, {"Home", "Library", "Collections", "Notes"}, 0);
        checkGolden(c, golden);
    };
    home.onShow(nav);
    withTabs(home, "tests/golden/home-account-chip.png");
    home.onGesture(tapOn(home.chipRect()), nav);
    REQUIRE(home.accountSheet());
    withTabs(home, "tests/golden/account-sheet.png");

    PadRig r;
    PinPadScreen pad = r.pad();
    pad.onShow(r.nav);
    pad.onGesture(tapOn(pad.keyRect("4")), r.nav);
    pad.onGesture(tapOn(pad.keyRect("2")), r.nav);
    pad.draw(c);
    checkGolden(c, "tests/golden/pin-pad.png");
    for (int i = 0; i < 6; ++i) r.type(pad, "0000");
    REQUIRE(pad.waiting());
    pad.draw(c);
    checkGolden(c, "tests/golden/pin-wait.png");
    PinPadScreen forgot = r.pad();
    forgot.onShow(r.nav);
    r.mono += 31'000;
    r.wall += 31'000;
    forgot.onShow(r.nav);
    forgot.onGesture(tapOn(forgot.forgotRect()), r.nav);
    forgot.draw(c);
    checkGolden(c, "tests/golden/pin-forgot.png");

    FakeHost removing;
    AccountSummary a = anna(false);
    a.active = true;
    removing.list = {pavel(), a};
    removing.list[0].active = false;
    RemoveAccountScreen remove(removing, 22);
    remove.onShow(nav);
    remove.draw(c);
    checkGolden(c, "tests/golden/remove-account.png");
}

TEST_CASE("account screens: goldens on the KT4 and the Scribe — the list and the PIN pad fit") {
    if (!fonts()) return;
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    FakeHost host;
    host.list = {pavel(), anna()};
    AccountSummary lena;
    lena.userId = 33;
    lena.name = "Lena";
    lena.items = 37;
    host.list.push_back(lena);
    for (const Metrics* m : {&kKt4, &kScribe}) {
        ScopedMetrics s(*m);
        const bool kt4 = m == &kKt4;
        Canvas c(m->width, m->height);
        AccountsScreen list(host);
        list.onShow(nav);
        for (size_t i = 0; i < list.shown().size(); ++i) CHECK(list.rowRect(i).y + list.rowRect(i).h <= m->height);
        list.draw(c);
        if (kt4) checkShrunk(c, "tests/golden/accounts-list-kt4.png");
        else checkGolden(c, "tests/golden/accounts-list-scribe.png");
        PadRig r;
        PinPadScreen pad = r.pad();
        pad.onShow(r.nav);
        for (const char* key : {"1", "9", "⌫", "0", "OK"}) CHECK(pad.keyRect(key).y + pad.keyRect(key).h <= m->height);
        CHECK(pad.anotherRect().y + pad.anotherRect().h <= m->height);
        pad.onGesture(tapOn(pad.keyRect("4")), r.nav);
        pad.draw(c);
        if (kt4) checkShrunk(c, "tests/golden/pin-pad-kt4.png");
        else checkGolden(c, "tests/golden/pin-pad-scribe.png");
    }
}

// ---- the final review's fixes ----------------------------------------------------------------------

TEST_CASE("account screens: two accounts removed one after the other leave no config.json; the next start's Setup is empty") {
    Rig k;
    Config stale;
    stale.userId = 11;
    stale.apiKey = "key11";
    REQUIRE(saveConfig(k.paths.configDir, stale));  // what a Setup used to write besides the credentials
    k.openAt(11);
    k.app.push(std::make_unique<RemoveAccountScreen>(*k.ui, 22));
    k.tap(k.topAs<RemoveAccountScreen>()->removeRect());
    CHECK(fs::exists(k.paths.configFile()));  // 11's: 11 is still here
    k.app.push(std::make_unique<RemoveAccountScreen>(*k.ui, 11));
    k.top()->onGesture(tapOn(k.topAs<RemoveAccountScreen>()->removeRect()), k.app);
    REQUIRE(k.settle());
    k.app.step(0);
    REQUIRE(k.topAs<SetupScreen>());
    CHECK(k.topAs<SetupScreen>()->config().userId == 0);
    CHECK_FALSE(fs::exists(k.paths.configFile()));
    CHECK_FALSE(fs::exists(k.paths.credentialsFile(11)));
    CHECK_FALSE(fs::exists(k.paths.credentialsFile(22)));
    // The next start: nothing of anyone's key to fill Setup with.
    Paths p = k.paths;
    REQUIRE(openAccountLayout(p) == std::optional<int64_t>(0));
    const Startup s = startupConfig(p);
    CHECK_FALSE(s.config);
    CHECK_FALSE(firstSetupKeepsConfigFile(p));
}

TEST_CASE("account screens: locked on sleep, the open account shows its name and lock only, in the lock pad's Use another account too") {
    Rig k;
    lockedPavel(k);
    for (const AccountSummary& a : k.ui->accounts(11)) {
        CHECK(a.locked);  // Pavel open but locked; Anna not open, with a PIN
        CHECK(a.items == 0);
        CHECK(a.keyLine.empty());
        CHECK(a.bytes == -1);
        CHECK_FALSE(a.measuring);
    }
    useAnother(k, 22);
    REQUIRE(padFor(k, "Anna"));
    auto* pad = k.topAs<PinPadScreen>();
    k.tap(pad->anotherRect());
    REQUIRE(pad->chooser());  // Pavel is locked: a chooser, not straight back
    const auto& rows = pad->chooser()->rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].userId == 11);
    CHECK(rows[0].locked);
    CHECK(rows[0].line.empty());
    // Chosen: its own lock pad; unlocked with its PIN, it shows everything again.
    k.tap(pad->chooser()->rowRect({0, 0, k.display.w, k.display.h}, 0));
    k.app.step(0);
    REQUIRE(padFor(k, "Pavel"));
    k.typePin("1357");
    k.app.step(0);
    for (const AccountSummary& a : k.ui->accounts())
        if (a.userId == 11) {
            CHECK_FALSE(a.locked);
            CHECK_FALSE(a.keyLine.empty());
        }
}

TEST_CASE("account screens: an account listed again after an unreadable list opens only with its key, never a PIN") {
    Rig k;
    AccountsState st = *readAccounts(k.paths.accountsFile());
    st.accounts[1].needsKey = true;  // as rebuildAccounts lists Anna
    REQUIRE(writeAccounts(k.paths.accountsFile(), st));
    k.openAt(11);
    for (const AccountSummary& a : k.ui->accounts())
        if (a.userId == 22) CHECK(a.locked);
    k.ui->switchTo(22);
    k.app.step(0);
    auto* pad = k.topAs<PinPadScreen>();
    REQUIRE(pad);
    CHECK(pad->stage() == PinPadScreen::Stage::Forgot);  // no PIN to enter: the key
    CHECK(pad->prompt().find("opens only with its Zotero API key") != std::string::npos);
    CHECK(pad->forgotRect().empty());
    std::ofstream(k.dir / "reset-pin.txt") << "key11";  // another account's key
    k.tap(pad->checkFileRect());
    CHECK(pad->message() == "This key isn't this account's");
    CHECK(k.session()->userId() == 11);
    std::ofstream(k.dir / "reset-pin.txt") << "key22\n";
    k.tap(pad->checkFileRect());
    REQUIRE(k.settle());
    k.app.step(0);
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 22);
    CHECK(k.homeOf() == 22);
    CHECK_FALSE(k.entry(22)->needsKey);
    CHECK_FALSE(k.entry(22)->pin);
}

TEST_CASE("account screens: at startup, an account that needs its key offers Use another account beside the key") {
    Rig k;
    AccountsState st = *readAccounts(k.paths.accountsFile());
    st.accounts[0].needsKey = true;
    REQUIRE(writeAccounts(k.paths.accountsFile(), st));
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.app.step(0);
    auto* pad = k.topAs<PinPadScreen>();
    REQUIRE(pad);
    CHECK_FALSE(k.session());
    REQUIRE_FALSE(pad->anotherRect().empty());
    k.tap(pad->anotherRect());
    REQUIRE(k.settle());
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 22);
    CHECK_FALSE(fs::exists(accountPaths(k.paths, 11).dbPath));
}

TEST_CASE("account screens: an account's size is measured off the screen thread, '…' until known, then kept for the run") {
    Rig k;
    k.openAt(11);
    const auto first = k.ui->accounts(22);
    const AccountSummary* anna = nullptr;
    for (const auto& a : first)
        if (a.userId == 22) anna = &a;
    REQUIRE(anna);
    CHECK((anna->measuring || anna->bytes > 0));
    int64_t bytes = -1;
    REQUIRE(eventually([&] {
        for (const auto& a : k.ui->accounts(22))
            if (a.userId == 22 && !a.measuring) bytes = a.bytes;
        return bytes >= 0;
    }));
    CHECK(bytes == static_cast<int64_t>(std::string("a file of Anna").size()));
    std::ofstream(fs::path(accountPaths(k.paths, 22).filesDir) / "MARK" / "more.pdf") << "more";
    for (const auto& a : k.ui->accounts(22))
        if (a.userId == 22) CHECK(a.bytes == bytes);  // not walked again this run
    // The Remove screen says "…" while it is measured, then the size.
    k.app.push(std::make_unique<RemoveAccountScreen>(*k.ui, 11));
    k.app.step(0);
    auto* remove = k.topAs<RemoveAccountScreen>();
    REQUIRE(remove);
    REQUIRE(eventually([&] {
        k.app.step(0);
        return !remove->shown().measuring;
    }));
    CHECK(remove->removeLabel().find("…") == std::string::npos);
    CHECK(remove->removeLabel().rfind("Remove Pavel and delete ", 0) == 0);
}

TEST_CASE("account screens: an account confirmed by Setup while the open account locks waits for its PIN; another account chosen drops it") {
    for (bool unlockOpen : {true, false}) {
        CAPTURE(unlockOpen);
        Rig k;
        k.setPin(11, "1357");
        k.setPin(22, "2468");
        REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
        k.app.step(0);
        k.typePin("1357");
        REQUIRE(k.settle());
        k.ui->addAccount(k.app);
        auto* setup = k.topAs<SetupScreen>();
        REQUIRE(setup);
        setup->onGesture(tapOn(SetupScreen::fieldRect(0)), k.app);
        setup->onKey(KeyEvent{KeyEvent::Code::Text, "33"}, k.app);
        setup->onGesture(tapOn(SetupScreen::fieldRect(1)), k.app);
        setup->onKey(KeyEvent{KeyEvent::Code::Text, "key33"}, k.app);
        setup->onGesture(tapOn(SetupScreen::connectRect()), k.app);
        // The check's answer is taken (its result deferred to the end of the step)...
        REQUIRE(eventually([&] {
            setup->onTick(k.app);
            return !setup->checking();
        }, 10000));
        k.ui->lockNow();  // ...and the Kindle sleeps before it is acted on
        k.app.step(0);
        REQUIRE(padFor(k, "Pavel"));
        CHECK(k.session()->userId() == 11);
        CHECK_FALSE(k.entry(33));
        CHECK_FALSE(loadCredentials(k.paths, 33));
        if (unlockOpen) {
            k.typePin("1357");
            REQUIRE(eventually([&] {
                k.app.step(0);
                return k.session() && k.session()->userId() == 33 && !k.switcher->busy();
            }, 10000));
            CHECK(k.entry(33));
        } else {
            useAnother(k, 22);
            REQUIRE(padFor(k, "Anna"));
            k.typePin("2468");
            REQUIRE(k.settle());
            k.app.step(0);
            CHECK(k.session()->userId() == 22);
            CHECK_FALSE(k.entry(33));
            CHECK_FALSE(loadCredentials(k.paths, 33));
            CHECK(k.app.lastToast() == "user33 wasn't added; add it again in Settings → Accounts");
        }
    }
}

TEST_CASE("account screens: a dropped key Zotero can't check now (429, 5xx) is 'will try again', not 'isn't theirs'") {
    Rig k;
    k.openAt(11);
    k.online = true;
    std::ofstream(k.paths.droppedConfig) << R"({"userId":22,"apiKey":"new22"})";
    k.ownerAnswer = [](const Config&) { return UserError{ErrorKind::RateLimited, "Zotero asks to wait.", "HTTP 429"}; };
    k.ui->checkDroppedKey();
    REQUIRE(eventually([&] {
        k.app.step(0);
        k.ui->poll();
        return !k.app.lastToast().empty();
    }, 10000));
    CHECK(k.app.lastToast() == "Couldn't check the key for Anna — will try again");
    CHECK(fs::exists(k.paths.droppedConfig));
    CHECK(loadCredentials(k.paths, 22)->apiKey == "key22");
}

TEST_CASE("account screens: a listed account whose folder is gone is never opened as a new, empty library") {
    Rig k;
    k.openAt(11);
    fs::remove_all(accountPaths(k.paths, 22).root);
    k.ui->switchTo(22);
    REQUIRE(k.settle());
    k.app.step(0);
    CHECK_FALSE(k.session());
    CHECK(k.topAs<AccountErrorScreen>());
    CHECK_FALSE(fs::exists(accountPaths(k.paths, 22).root));
}
