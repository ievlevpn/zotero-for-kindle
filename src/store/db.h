#pragma once
// A thin RAII layer over SQLite: open (WAL), exec, prepared statements. Failures throw DbError
// carrying SQLite's primary result code, so callers can tell "disk full" from other faults.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace zk {

class DbError : public std::runtime_error {
public:
    DbError(const std::string& what, int code) : std::runtime_error(what), code_(code) {}
    int code() const { return code_; }  // primary SQLite result code (SQLITE_FULL, ...)
    bool diskFull() const;

private:
    int code_;
};

class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql);
    Stmt(Stmt&&) noexcept;
    Stmt& operator=(Stmt&&) = delete;
    Stmt(const Stmt&) = delete;
    ~Stmt();

    Stmt& bind(int index, const std::string& v);  // 1-based, like SQLite
    Stmt& bind(int index, int64_t v);
    Stmt& bindNull(int index);
    Stmt& bindReal(int index, double v);
    bool step();  // true while a row is available; throws on error
    void reset();

    std::string text(int col) const;  // 0-based
    int64_t integer(int col) const;
    double real(int col) const;

private:
    sqlite3* db_;
    sqlite3_stmt* st_ = nullptr;
};

class Db {
public:
    static Db open(const std::string& path);  // creates the file; WAL mode; foreign keys off
    Db(Db&&) noexcept;
    Db& operator=(Db&&) = delete;
    Db(const Db&) = delete;
    ~Db();

    void exec(const std::string& sql);
    Stmt prepare(const std::string& sql);
    int64_t userVersion();
    void setUserVersion(int64_t);
    const std::string& path() const { return path_; }
    sqlite3* raw() { return db_; }

private:
    Db(sqlite3* db, std::string path) : db_(db), path_(std::move(path)) {}
    sqlite3* db_;
    std::string path_;
};

}  // namespace zk
