// SPDX-License-Identifier: MIT

#include "sql_connection.hpp"

#include <sqlite3.h>
#include <utility>

namespace tessera::appfw::sql::detail {

namespace {

constexpr std::size_t kStatementCacheLimit = 32;
// How many SQLite VM instructions run between two polls of the cancellation token.
constexpr int kProgressInterval = 1000;

int poll_cancellation(void* context) {
    return static_cast<const runtime::CancellationToken*>(context)->cancelled() ? 1 : 0;
}

// Installs the cancellation poll for the duration of one call and removes it after, so a stale
// pointer to a finished call's token is never left on the connection.
class ProgressScope {
public:
    ProgressScope(sqlite3* db, const runtime::CancellationToken& token)
        : db_(db) {
        sqlite3_progress_handler(db_, kProgressInterval, &poll_cancellation,
                                 const_cast<runtime::CancellationToken*>(&token));
    }
    ~ProgressScope() {
        sqlite3_progress_handler(db_, 0, nullptr, nullptr);
    }
    ProgressScope(const ProgressScope&) = delete;
    ProgressScope& operator=(const ProgressScope&) = delete;

private:
    sqlite3* db_;
};

// Leaves a cached statement reusable whatever path left the call.
class StatementReset {
public:
    explicit StatementReset(sqlite3_stmt* stmt)
        : stmt_(stmt) {}
    ~StatementReset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
    StatementReset(const StatementReset&) = delete;
    StatementReset& operator=(const StatementReset&) = delete;

private:
    sqlite3_stmt* stmt_;
};

[[nodiscard]] bool only_space_or_semicolon(const char* text) {
    for (; *text != '\0'; ++text) {
        const char c = *text;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] SqlErrorCode classify(int rc) {
    switch (rc & 0xff) {
    case SQLITE_CONSTRAINT:
        return SqlErrorCode::Constraint;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return SqlErrorCode::Busy;
    default:
        return SqlErrorCode::Sqlite;
    }
}

[[nodiscard]] Value read_column(sqlite3_stmt* stmt, int column) {
    switch (sqlite3_column_type(stmt, column)) {
    case SQLITE_INTEGER:
        return Value(static_cast<std::int64_t>(sqlite3_column_int64(stmt, column)));
    case SQLITE_FLOAT:
        return Value(sqlite3_column_double(stmt, column));
    case SQLITE_TEXT: {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
        const int size = sqlite3_column_bytes(stmt, column);
        return Value(std::string(text != nullptr ? text : "", static_cast<std::size_t>(size)));
    }
    case SQLITE_BLOB: {
        const auto* bytes = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt, column));
        const int size = sqlite3_column_bytes(stmt, column);
        return Value(bytes != nullptr ? Blob(bytes, bytes + size) : Blob());
    }
    default:
        return Value();
    }
}

// Binds one value without converting it. Returns the SQLite result code.
[[nodiscard]] int bind_value(sqlite3_stmt* stmt, int index, const Value& value) {
    if (value.is_null()) {
        return sqlite3_bind_null(stmt, index);
    }
    if (value.is_int64()) {
        return sqlite3_bind_int64(stmt, index, *value.as_int64());
    }
    if (value.is_double()) {
        return sqlite3_bind_double(stmt, index, *value.as_double());
    }
    if (value.is_string()) {
        const std::string_view text = *value.as_string();
        return sqlite3_bind_text64(stmt, index, text.data(), text.size(), SQLITE_TRANSIENT,
                                   SQLITE_UTF8);
    }
    const auto blob = *value.as_blob();
    if (blob.empty()) {
        return sqlite3_bind_zeroblob(stmt, index, 0);
    }
    return sqlite3_bind_blob64(stmt, index, blob.data(), blob.size(), SQLITE_TRANSIENT);
}

} // namespace

SqlError make_error(SqlErrorCode code, std::string_view step, std::string message, int native) {
    return SqlError{code, step, native, std::move(message)};
}

SqlError cancelled_error(std::string_view step) {
    return make_error(SqlErrorCode::Cancelled, step, "cancelled");
}

Connection::~Connection() {
    close();
}

SqlError Connection::last_error(std::string_view step, int rc,
                                const runtime::CancellationToken& token) const {
    if ((rc & 0xff) == SQLITE_INTERRUPT && token.cancelled()) {
        return cancelled_error(step);
    }
    const int extended = db_ != nullptr ? sqlite3_extended_errcode(db_) : rc;
    return make_error(classify(rc), step, db_ != nullptr ? sqlite3_errmsg(db_) : "no connection",
                      extended);
}

std::expected<void, SqlError> Connection::open(const std::string& path,
                                               const OpenOptions& options) {
    if (db_ != nullptr) {
        return std::unexpected(make_error(SqlErrorCode::AlreadyOpen, "open", "already open"));
    }
    options_ = options;
#if defined(__EMSCRIPTEN__)
    constexpr bool kPlatformMemoryOnly = true;
#else
    constexpr bool kPlatformMemoryOnly = false;
#endif
    if ((options.memory_only || kPlatformMemoryOnly) && path != ":memory:" && !path.empty()) {
        return std::unexpected(make_error(SqlErrorCode::Unsupported, "open",
                                          "only :memory: databases are available here"));
    }
    sqlite3* handle = nullptr;
    // FULLMUTEX is not needed (the owner serializes) but costs nothing next to I/O and keeps a
    // misuse from corrupting memory.
    const int rc = sqlite3_open_v2(
        path.c_str(), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        options.vfs.empty() ? nullptr : options.vfs.c_str());
    if (rc != SQLITE_OK) {
        SqlError error = make_error(
            classify(rc), "open", handle != nullptr ? sqlite3_errmsg(handle) : "out of memory", rc);
        sqlite3_close_v2(handle);
        return std::unexpected(std::move(error));
    }
    db_ = handle;
    sqlite3_busy_timeout(db_, options.busy_timeout_ms);

    const runtime::CancellationToken never;
    const bool memory = path == ":memory:" || path.empty();
    std::string pragmas;
    if (options.foreign_keys) {
        pragmas += "PRAGMA foreign_keys=ON;";
    }
    if (options.wal && !memory) {
        pragmas += "PRAGMA journal_mode=WAL;";
    }
    if (!pragmas.empty()) {
        if (auto applied = script(pragmas, never); !applied) {
            SqlError error = applied.error();
            close();
            return std::unexpected(std::move(error));
        }
    }
    return {};
}

void Connection::finalize_all() noexcept {
    for (auto& [sql, stmt] : cache_) {
        (void)sql;
        sqlite3_finalize(stmt);
    }
    cache_.clear();
}

void Connection::close() noexcept {
    if (db_ == nullptr) {
        return;
    }
    finalize_all();
    sqlite3_close_v2(db_);
    db_ = nullptr;
}

std::expected<sqlite3_stmt*, SqlError> Connection::acquire(std::string_view sql) {
    if (const auto found = cache_.find(std::string(sql)); found != cache_.end()) {
        return found->second;
    }
    // The text is copied so the buffer is NUL-terminated for the tail check.
    const std::string text(sql);
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    const int rc =
        sqlite3_prepare_v2(db_, text.c_str(), static_cast<int>(text.size() + 1), &stmt, &tail);
    if (rc != SQLITE_OK) {
        return std::unexpected(last_error("prepare", rc, {}));
    }
    if (stmt == nullptr) {
        return std::unexpected(
            make_error(SqlErrorCode::InvalidArgument, "prepare", "empty statement"));
    }
    if (tail != nullptr && !only_space_or_semicolon(tail)) {
        sqlite3_finalize(stmt);
        return std::unexpected(
            make_error(SqlErrorCode::InvalidArgument, "prepare",
                       "more than one statement; use a migration or a transaction"));
    }
    if (cache_.size() >= kStatementCacheLimit) {
        finalize_all();
    }
    cache_.emplace(text, stmt);
    return stmt;
}

std::expected<ExecResult, SqlError> Connection::run(std::string_view sql, const Binds& binds,
                                                    const runtime::CancellationToken& token,
                                                    Rows* rows) {
    if (db_ == nullptr) {
        return std::unexpected(
            make_error(SqlErrorCode::NotOpen, "prepare", "database is not open"));
    }
    if (token.cancelled()) {
        return std::unexpected(cancelled_error("prepare"));
    }
    auto prepared = acquire(sql);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    sqlite3_stmt* stmt = *prepared;
    const StatementReset reset(stmt);

    if (static_cast<std::size_t>(sqlite3_bind_parameter_count(stmt)) != binds.size()) {
        return std::unexpected(
            make_error(SqlErrorCode::BindCount, "bind",
                       "statement has " + std::to_string(sqlite3_bind_parameter_count(stmt)) +
                           " parameters, " + std::to_string(binds.size()) + " supplied"));
    }
    for (std::size_t i = 0; i < binds.size(); ++i) {
        if (const int rc = bind_value(stmt, static_cast<int>(i + 1), binds[i]); rc != SQLITE_OK) {
            return std::unexpected(last_error("bind", rc, token));
        }
    }
    if (rows != nullptr) {
        const int count = sqlite3_column_count(stmt);
        rows->columns.reserve(static_cast<std::size_t>(count));
        for (int c = 0; c < count; ++c) {
            rows->columns.emplace_back(sqlite3_column_name(stmt, c));
        }
    }

    const ProgressScope progress(db_, token);
    for (;;) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            return std::unexpected(last_error("step", rc, token));
        }
        if (rows == nullptr) {
            continue;
        }
        if (rows->rows.size() >= options_.max_rows) {
            return std::unexpected(
                make_error(SqlErrorCode::TooLarge, "step", "result exceeds max_rows"));
        }
        Row row;
        const int count = sqlite3_column_count(stmt);
        row.reserve(static_cast<std::size_t>(count));
        for (int c = 0; c < count; ++c) {
            row.push_back(read_column(stmt, c));
        }
        rows->rows.push_back(std::move(row));
    }
    return ExecResult{static_cast<std::int64_t>(sqlite3_changes64(db_)),
                      static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_))};
}

std::expected<void, SqlError> Connection::script(const std::string& sql,
                                                 const runtime::CancellationToken& token) {
    if (db_ == nullptr) {
        return std::unexpected(make_error(SqlErrorCode::NotOpen, "step", "database is not open"));
    }
    if (token.cancelled()) {
        return std::unexpected(cancelled_error("step"));
    }
    const ProgressScope progress(db_, token);
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(last_error("step", rc, token));
    }
    return {};
}

void Connection::rollback_if_open() noexcept {
    if (db_ != nullptr && sqlite3_get_autocommit(db_) == 0) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

} // namespace tessera::appfw::sql::detail
