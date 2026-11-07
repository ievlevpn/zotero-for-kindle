#include "store/db.h"

#include <sqlite3.h>

namespace zk {

namespace {

[[noreturn]] void fail(sqlite3* db, int rc, const std::string& what) {
    const char* msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    throw DbError(what + ": " + msg, rc & 0xff);
}

}  // namespace

bool DbError::diskFull() const { return code_ == SQLITE_FULL; }

Stmt::Stmt(sqlite3* db, const std::string& sql) : db_(db) {
    int rc = sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &st_, nullptr);
    if (rc != SQLITE_OK) fail(db, rc, "prepare");
}

Stmt::Stmt(Stmt&& o) noexcept : db_(o.db_), st_(o.st_) { o.st_ = nullptr; }

Stmt::~Stmt() { sqlite3_finalize(st_); }

Stmt& Stmt::bind(int index, const std::string& v) {
    int rc = sqlite3_bind_text(st_, index, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) fail(db_, rc, "bind");
    return *this;
}

Stmt& Stmt::bind(int index, int64_t v) {
    int rc = sqlite3_bind_int64(st_, index, v);
    if (rc != SQLITE_OK) fail(db_, rc, "bind");
    return *this;
}

Stmt& Stmt::bindNull(int index) {
    int rc = sqlite3_bind_null(st_, index);
    if (rc != SQLITE_OK) fail(db_, rc, "bind");
    return *this;
}

bool Stmt::step() {
    int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    sqlite3_reset(st_);
    fail(db_, rc, "step");
}

void Stmt::reset() {
    sqlite3_reset(st_);
    sqlite3_clear_bindings(st_);
}

std::string Stmt::text(int col) const {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st_, col));
    return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(st_, col))) : std::string{};
}

int64_t Stmt::integer(int col) const { return sqlite3_column_int64(st_, col); }

double Stmt::real(int col) const { return sqlite3_column_double(st_, col); }

Db Db::open(const std::string& path) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        sqlite3_close(db);
        throw DbError("Can't open the database at " + path + ": " + msg, rc & 0xff);
    }
    Db out(db, path);
    try {
        sqlite3_busy_timeout(db, 5000);
        out.exec("PRAGMA journal_mode=WAL");
        out.exec("PRAGMA synchronous=NORMAL");
    } catch (const DbError& e) {
        throw DbError("Can't open the database at " + path + ": " + e.what(), e.code());
    }
    return out;
}

Db::Db(Db&& o) noexcept : db_(o.db_), path_(std::move(o.path_)) { o.db_ = nullptr; }

Db::~Db() { sqlite3_close_v2(db_); }

void Db::exec(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errstr(rc);
        sqlite3_free(err);
        throw DbError(msg, sqlite3_extended_errcode(db_) & 0xff);
    }
}

Stmt Db::prepare(const std::string& sql) { return Stmt(db_, sql); }

int64_t Db::userVersion() {
    Stmt st = prepare("PRAGMA user_version");
    st.step();
    return st.integer(0);
}

void Db::setUserVersion(int64_t v) { exec("PRAGMA user_version = " + std::to_string(v)); }

}  // namespace zk
