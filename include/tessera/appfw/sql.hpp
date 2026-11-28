// SPDX-License-Identifier: MIT
//
// The opt-in SQLite module (issue #837; docs/design/app-framework-open-questions.md section 4,
// docs/design/storage-sqlite.md). It is the `tessera_appfw_storage_sqlite` static library: built
// only with -DTESSERA_APPFW_SQLITE=ON, never linked by default.
//
// The module's one rule is threading. SQLite's API blocks, so every call here only *posts* work to
// an `IoTaskExecutor`; the connection is touched only inside that work body and the completion runs
// on the thread that drains the completion queue. A caller on the UI thread cannot block on a query
// by using this module, which is the point of owning the adapter.
//
// One `Database` owns one connection. The connection is serialized by the database itself, so two
// operations never run on it at once even when the executor has several workers; ordering between
// operations is the order they reach the connection, so a caller that needs "B after A" starts B
// from A's completion (or puts both in one transaction).
//
// Typed values: `Value` is null, int64, double, string or blob. Binding never converts, a result
// column keeps the type SQLite stored, and reading it as another type is a `TypeMismatch` error.
//
// Cancellation: an operation whose token is cancelled before its body starts does nothing. While a
// statement runs the module polls the token from SQLite's progress handler and aborts the statement
// (SQLite reports it as an interrupt; the module reports `Cancelled`). A transaction in flight is
// rolled back. This is the same abort as `sqlite3_interrupt` without its lost-wakeup window (an
// interrupt posted between two statements is dropped by SQLite).
//
// Submit contract (the same one `Storage` follows): a method returns `false` when it did not start
// the operation (the executor refused it) and then the completion is never invoked. When it returns
// `true` the completion runs exactly once.

#pragma once

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/io_task_executor.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tessera::appfw::sql {

enum class SqlErrorCode : std::uint8_t {
    None = 0,
    Cancelled,       // the token was cancelled; a transaction was rolled back
    NotOpen,         // no open connection (never opened, or closed)
    AlreadyOpen,     // open() on a database that is already open
    InvalidArgument, // empty or multi-statement SQL, unordered migrations
    BindCount,       // the number of binds differs from the statement's parameters
    TypeMismatch,    // a typed read of a column holding another type
    TooLarge,        // the result exceeds OpenOptions::max_rows
    Constraint,      // a UNIQUE/NOT NULL/CHECK/FOREIGN KEY violation
    Busy,            // the database file is locked by another connection past the busy timeout
    Migration,       // a migration step failed (rolled back) or the file is newer than the code
    Sqlite,          // any other engine error; see `native`
};

[[nodiscard]] std::string_view sql_error_code_name(SqlErrorCode code) noexcept;

struct SqlError {
    SqlErrorCode code = SqlErrorCode::Sqlite;
    // The step that failed ("open", "prepare", "bind", "step", "begin", "commit", "migrate").
    std::string_view step;
    // SQLite's extended result code, or 0 when the failure is the module's own.
    int native = 0;
    std::string message;

    [[nodiscard]] bool operator==(const SqlError&) const = default;
};

using Blob = std::vector<std::uint8_t>;

// One typed SQL value. Default-constructed is NULL.
class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(std::int64_t v)
        : data_(v) {}
    Value(int v)
        : data_(static_cast<std::int64_t>(v)) {}
    Value(double v)
        : data_(v) {}
    Value(std::string v)
        : data_(std::move(v)) {}
    Value(std::string_view v)
        : data_(std::string(v)) {}
    Value(const char* v)
        : data_(std::string(v)) {}
    Value(Blob v)
        : data_(std::move(v)) {}

    [[nodiscard]] bool is_null() const noexcept {
        return std::holds_alternative<std::monostate>(data_);
    }
    [[nodiscard]] bool is_int64() const noexcept {
        return std::holds_alternative<std::int64_t>(data_);
    }
    [[nodiscard]] bool is_double() const noexcept {
        return std::holds_alternative<double>(data_);
    }
    [[nodiscard]] bool is_string() const noexcept {
        return std::holds_alternative<std::string>(data_);
    }
    [[nodiscard]] bool is_blob() const noexcept {
        return std::holds_alternative<Blob>(data_);
    }

    // Typed reads. Each fails with `TypeMismatch` unless the value has exactly that type: an
    // integer is not a double and text is not a blob.
    [[nodiscard]] std::expected<std::int64_t, SqlError> as_int64() const;
    [[nodiscard]] std::expected<double, SqlError> as_double() const;
    [[nodiscard]] std::expected<std::string_view, SqlError> as_string() const;
    [[nodiscard]] std::expected<std::span<const std::uint8_t>, SqlError> as_blob() const;

    [[nodiscard]] bool operator==(const Value&) const = default;

private:
    std::variant<std::monostate, std::int64_t, double, std::string, Blob> data_;
};

using Binds = std::vector<Value>;
using Row = std::vector<Value>;

struct Rows {
    std::vector<std::string> columns;
    std::vector<Row> rows;
};

struct ExecResult {
    std::int64_t changes = 0;
    std::int64_t last_insert_rowid = 0;
};

struct OpenOptions {
    // Rollback-journal databases block readers behind a writer; WAL is the default for files.
    // Ignored for `:memory:`.
    bool wal = true;
    bool foreign_keys = true;
    // How long a locked file is retried inside the work body (never on the UI thread).
    int busy_timeout_ms = 5000;
    // Cap on rows one query may return; more is `TooLarge`.
    std::size_t max_rows = 100'000;
};

// A versioned schema step. `sql` may hold several statements. Versions are positive and strictly
// increasing; the database's `PRAGMA user_version` records the last applied one.
struct Migration {
    std::uint32_t version = 0;
    std::string sql;
};

// Synchronous statement access inside `Database::transaction`'s body. The body already runs on the
// I/O executor, so these calls block that thread only.
class Transaction {
public:
    virtual ~Transaction() = default;
    virtual std::expected<ExecResult, SqlError> execute(std::string_view sql,
                                                        const Binds& binds = {}) = 0;
    virtual std::expected<Rows, SqlError> query(std::string_view sql, const Binds& binds = {}) = 0;
};

class Database {
public:
    using StatusCallback = std::function<void(std::expected<void, SqlError>)>;
    using ExecCallback = std::function<void(std::expected<ExecResult, SqlError>)>;
    using RowsCallback = std::function<void(std::expected<Rows, SqlError>)>;
    using VersionCallback = std::function<void(std::expected<std::uint32_t, SqlError>)>;
    using TransactionBody = std::function<std::expected<void, SqlError>(Transaction&)>;

    // `executor` is borrowed and must outlive every operation this database posts.
    explicit Database(runtime::IoTaskExecutor& executor);
    // Closes a still-open connection (a short, non-waiting close of an idle handle). Prefer
    // `close()`, which does it on the executor.
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // `path` is a file path or `:memory:`. Opening is the only operation valid on a closed
    // database.
    bool open(std::string path, OpenOptions options, runtime::CancellationToken token,
              StatusCallback completion);
    bool close(StatusCallback completion);

    // One statement that returns no rows you need (INSERT/UPDATE/DELETE/DDL).
    bool execute(std::string sql, Binds binds, runtime::CancellationToken token,
                 ExecCallback completion);
    bool query(std::string sql, Binds binds, runtime::CancellationToken token,
               RowsCallback completion);

    // BEGIN IMMEDIATE; run `body`; COMMIT when it returns a value, ROLLBACK when it returns an
    // error or the token is cancelled. `body` runs on the executor thread.
    bool transaction(TransactionBody body, runtime::CancellationToken token,
                     StatusCallback completion);

    // Applies the migrations newer than `PRAGMA user_version`, in order, each in its own
    // transaction. A failing step rolls back and leaves `user_version` at the last good step. The
    // completion carries the resulting version.
    bool migrate(std::vector<Migration> migrations, runtime::CancellationToken token,
                 VersionCallback completion);

    // The version SQLite reports for `PRAGMA user_version`.
    bool user_version(runtime::CancellationToken token, VersionCallback completion);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace tessera::appfw::sql
