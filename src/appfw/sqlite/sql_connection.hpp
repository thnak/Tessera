// SPDX-License-Identifier: MIT
//
// Internal to `tessera_appfw_storage_sqlite`: the blocking connection wrapper. Everything here is
// synchronous and runs only inside an I/O-executor work body; `Database` (sql_database.cpp) is what
// serializes access and posts the work. This header is the only place that names `sqlite3`, so no
// SQLite header reaches a public include.

#pragma once

#include "tessera/appfw/sql.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>

struct sqlite3;
struct sqlite3_stmt;

namespace tessera::appfw::sql::detail {

[[nodiscard]] SqlError make_error(SqlErrorCode code, std::string_view step, std::string message,
                                  int native = 0);
[[nodiscard]] SqlError cancelled_error(std::string_view step);

// One SQLite connection plus a small prepared-statement cache (a statement is prepared once and
// re-bound on every run). Not thread-safe: the owner serializes calls.
class Connection {
public:
    Connection() = default;
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] bool is_open() const noexcept {
        return db_ != nullptr;
    }

    std::expected<void, SqlError> open(const std::string& path, const OpenOptions& options);
    void close() noexcept;

    // Runs one statement. `rows` may be null (the result rows are then discarded).
    std::expected<ExecResult, SqlError> run(std::string_view sql, const Binds& binds,
                                            const runtime::CancellationToken& token, Rows* rows);
    // Runs a script of several statements with no binds (migrations).
    std::expected<void, SqlError> script(const std::string& sql,
                                         const runtime::CancellationToken& token);
    // ROLLBACK when a transaction is open; harmless otherwise.
    void rollback_if_open() noexcept;

    [[nodiscard]] std::size_t cached_statements() const noexcept {
        return cache_.size();
    }

private:
    std::expected<sqlite3_stmt*, SqlError> acquire(std::string_view sql);
    void finalize_all() noexcept;
    [[nodiscard]] SqlError last_error(std::string_view step, int rc,
                                      const runtime::CancellationToken& token) const;

    sqlite3* db_ = nullptr;
    OpenOptions options_;
    std::unordered_map<std::string, sqlite3_stmt*> cache_;
};

} // namespace tessera::appfw::sql::detail
