#include <doctest/doctest.h>

extern "C" {
#include <md5.h>
}
#include <miniz.h>
#include <sqlite3.h>

#include <cstdio>
#include <string>

TEST_CASE("sqlite fts5 with unicode61 matches case-insensitively across scripts") {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
    char* err = nullptr;
    int rc = sqlite3_exec(db,
        "CREATE VIRTUAL TABLE t USING fts5(x, tokenize='unicode61 remove_diacritics 2');"
        "INSERT INTO t(x) VALUES ('Привет мир'), ('hello world');",
        nullptr, nullptr, &err);
    INFO((err ? err : ""));
    REQUIRE(rc == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT x FROM t WHERE t MATCH '\"привет\"'", -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))) == "Привет мир");
    CHECK(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

TEST_CASE("md5 of the quick brown fox") {
    const std::string s = "The quick brown fox jumps over the lazy dog";
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, s.data(), s.size());
    unsigned char out[16];
    MD5_Final(out, &ctx);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", out[i]);
    CHECK(std::string(hex) == "9e107d9d372bb6826bd81d3542a419d6");
}

TEST_CASE("miniz extracts a committed zip fixture") {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_reader_init_file(&zip, "tests/data/zip/hello.zip", 0));
    REQUIRE(mz_zip_reader_get_num_files(&zip) == 1);
    mz_zip_archive_file_stat st;
    REQUIRE(mz_zip_reader_file_stat(&zip, 0, &st));
    CHECK(std::string(st.m_filename) == "hello.txt");
    size_t size = 0;
    void* p = mz_zip_reader_extract_to_heap(&zip, 0, &size, 0);
    REQUIRE(p != nullptr);
    CHECK(std::string(static_cast<char*>(p), size) == "hello\n");
    mz_free(p);
    mz_zip_reader_end(&zip);
}
