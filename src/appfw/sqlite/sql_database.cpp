// SPDX-License-Identifier: MIT

#include "sql_connection.hpp"
#include "tessera/appfw/sql.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace tessera::appfw::sql {

namespace {

using detail::cancelled_error;
using detail::Connection;
using detail::make_error;

[[nodiscard]] std::unexpected<SqlError> not_open(std::string_view step) {
    return std::unexpected(make_error(SqlErrorCode::NotOpen, step, "database is not open"));
}

// The statement access a transaction body gets: the live connection and the operation's token.
class ConnectionTransaction final : public Transaction {
public:
    ConnectionTransaction(Connection& connection, const runtime::CancellationToken& token)
        : connection_(connection),
          token_(token) {}

    std::expected<ExecResult, SqlError> execute(std::string_view sql, const Binds& binds) override {
        return connection_.run(sql, binds, token_, nullptr);
    }
    std::expected<Rows, SqlError> query(std::string_view sql, const Binds& binds) override {
        Rows rows;
        auto result = connection_.run(sql, binds, token_, &rows);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        return rows;
    }

private:
    Connection& connection_;
    const runtime::CancellationToken& token_;
};

// BEGIN IMMEDIATE ... COMMIT around `body`; rolls back on any failure and on cancellation.
template<typename Body>
std::expected<void, SqlError> in_transaction(Connection& connection,
                                             const runtime::CancellationToken& token, Body&& body) {
    if (auto begun = connection.run("BEGIN IMMEDIATE", {}, token, nullptr); !begun) {
        auto error = std::move(begun.error());
        error.step = "begin";
        return std::unexpected(std::move(error));
    }
    std::expected<void, SqlError> result = body();
    if (result && token.cancelled()) {
        result = std::unexpected(cancelled_error("commit"));
    }
    if (result) {
        // COMMIT must not be abandoned by a late cancellation, so it runs with an uncancellable
        // token.
        if (auto committed = connection.run("COMMIT", {}, runtime::CancellationToken{}, nullptr);
            !committed) {
            auto error = std::move(committed.error());
            error.step = "commit";
            result = std::unexpected(std::move(error));
        }
    }
    if (!result) {
        connection.rollback_if_open();
    }
    return result;
}

[[nodiscard]] std::expected<std::uint32_t, SqlError>
read_user_version(Connection& connection, const runtime::CancellationToken& token) {
    Rows rows;
    if (auto ran = connection.run("PRAGMA user_version", {}, token, &rows); !ran) {
        return std::unexpected(std::move(ran.error()));
    }
    if (rows.rows.size() != 1 || rows.rows[0].size() != 1) {
        return std::unexpected(make_error(SqlErrorCode::Sqlite, "step", "user_version unreadable"));
    }
    auto version = rows.rows[0][0].as_int64();
    if (!version) {
        return std::unexpected(std::move(version.error()));
    }
    return static_cast<std::uint32_t>(*version);
}

[[nodiscard]] std::expected<std::uint32_t, SqlError>
apply_migrations(Connection& connection, const std::vector<Migration>& migrations,
                 const runtime::CancellationToken& token) {
    std::uint32_t previous = 0;
    for (const Migration& migration : migrations) {
        if (migration.version == 0 || migration.version <= previous) {
            return std::unexpected(
                make_error(SqlErrorCode::InvalidArgument, "migrate",
                           "migration versions must be positive and increasing"));
        }
        previous = migration.version;
    }
    auto current = read_user_version(connection, token);
    if (!current) {
        return std::unexpected(std::move(current.error()));
    }
    if (previous != 0 && *current > previous) {
        return std::unexpected(make_error(SqlErrorCode::Migration, "migrate",
                                          "database version " + std::to_string(*current) +
                                              " is newer than the newest migration " +
                                              std::to_string(previous)));
    }
    std::uint32_t version = *current;
    for (const Migration& migration : migrations) {
        if (migration.version <= version) {
            continue;
        }
        auto step = in_transaction(connection, token, [&]() -> std::expected<void, SqlError> {
            if (auto applied = connection.script(migration.sql, token); !applied) {
                return std::unexpected(std::move(applied.error()));
            }
            // PRAGMA takes no binds; the value is an integer the module formatted itself.
            return connection
                .run("PRAGMA user_version = " + std::to_string(migration.version), {}, token,
                     nullptr)
                .transform([](ExecResult) {});
        });
        if (!step) {
            SqlError error = std::move(step.error());
            if (error.code != SqlErrorCode::Cancelled) {
                error.message =
                    "migration " + std::to_string(migration.version) + ": " + error.message;
                error.code = SqlErrorCode::Migration;
            }
            error.step = "migrate";
            return std::unexpected(std::move(error));
        }
        version = migration.version;
    }
    return version;
}

} // namespace

struct Database::Impl {
    explicit Impl(runtime::IoTaskExecutor& io)
        : executor(io) {}

    runtime::IoTaskExecutor& executor;
    // The connection's strand: held for the whole body of every operation.
    std::mutex mutex;
    Connection connection;

    // Posts `op` to the executor. The body runs under the mutex on an executor thread; the result
    // travels to the completion through a slot, so the completion never touches the connection.
    template<typename T, typename Op>
    static bool submit(const std::shared_ptr<Impl>& self, runtime::CancellationToken token, Op op,
                       std::function<void(std::expected<T, SqlError>)> done) {
        using Result = std::expected<T, SqlError>;
        auto slot = std::make_shared<std::optional<Result>>();
        runtime::IoTask task;
        task.work = [self, token, op = std::move(op), slot]() mutable {
            const std::scoped_lock lock(self->mutex);
            if (token.cancelled()) {
                *slot = std::unexpected(cancelled_error("start"));
                return;
            }
            try {
                *slot = op(self->connection, token);
            } catch (...) {
                // Exceptions never cross the executor boundary (ADR-025).
                *slot =
                    std::unexpected(make_error(SqlErrorCode::Sqlite, "step", "exception in body"));
            }
        };
        task.completion = [slot, done = std::move(done)]() {
            if (slot->has_value() && done) {
                done(std::move(**slot));
            }
        };
        return self->executor.post(std::move(task));
    }
};

Database::Database(runtime::IoTaskExecutor& executor)
    : impl_(std::make_shared<Impl>(executor)) {}

Database::~Database() = default;

bool Database::open(std::string path, OpenOptions options, runtime::CancellationToken token,
                    StatusCallback completion) {
    return Impl::submit<std::monostate>(
        impl_, std::move(token),
        [path = std::move(path), options](Connection& connection, const runtime::CancellationToken&)
            -> std::expected<std::monostate, SqlError> {
            return connection.open(path, options).transform([] { return std::monostate{}; });
        },
        [completion = std::move(completion)](std::expected<std::monostate, SqlError> result) {
            if (completion) {
                completion(result.transform([](std::monostate) {}));
            }
        });
}

bool Database::close(StatusCallback completion) {
    return Impl::submit<std::monostate>(
        impl_, {},
        [](Connection& connection,
           const runtime::CancellationToken&) -> std::expected<std::monostate, SqlError> {
            connection.close();
            return std::monostate{};
        },
        [completion = std::move(completion)](std::expected<std::monostate, SqlError> result) {
            if (completion) {
                completion(result.transform([](std::monostate) {}));
            }
        });
}

bool Database::execute(std::string sql, Binds binds, runtime::CancellationToken token,
                       ExecCallback completion) {
    return Impl::submit<ExecResult>(
        impl_, std::move(token),
        [sql = std::move(sql), binds = std::move(binds)](Connection& connection,
                                                         const runtime::CancellationToken& cancel) {
            return connection.run(sql, binds, cancel, nullptr);
        },
        std::move(completion));
}

bool Database::query(std::string sql, Binds binds, runtime::CancellationToken token,
                     RowsCallback completion) {
    return Impl::submit<Rows>(
        impl_, std::move(token),
        [sql = std::move(sql), binds = std::move(binds)](
            Connection& connection,
            const runtime::CancellationToken& cancel) -> std::expected<Rows, SqlError> {
            Rows rows;
            auto ran = connection.run(sql, binds, cancel, &rows);
            if (!ran) {
                return std::unexpected(std::move(ran.error()));
            }
            return rows;
        },
        std::move(completion));
}

bool Database::transaction(TransactionBody body, runtime::CancellationToken token,
                           StatusCallback completion) {
    return Impl::submit<std::monostate>(
        impl_, std::move(token),
        [body = std::move(body)](Connection& connection, const runtime::CancellationToken& cancel)
            -> std::expected<std::monostate, SqlError> {
            if (!connection.is_open()) {
                return not_open("begin");
            }
            ConnectionTransaction tx(connection, cancel);
            return in_transaction(connection, cancel, [&] { return body(tx); }).transform([] {
                return std::monostate{};
            });
        },
        [completion = std::move(completion)](std::expected<std::monostate, SqlError> result) {
            if (completion) {
                completion(result.transform([](std::monostate) {}));
            }
        });
}

bool Database::migrate(std::vector<Migration> migrations, runtime::CancellationToken token,
                       VersionCallback completion) {
    return Impl::submit<std::uint32_t>(
        impl_, std::move(token),
        [migrations = std::move(migrations)](
            Connection& connection,
            const runtime::CancellationToken& cancel) -> std::expected<std::uint32_t, SqlError> {
            if (!connection.is_open()) {
                return not_open("migrate");
            }
            return apply_migrations(connection, migrations, cancel);
        },
        std::move(completion));
}

bool Database::user_version(runtime::CancellationToken token, VersionCallback completion) {
    return Impl::submit<std::uint32_t>(
        impl_, std::move(token),
        [](Connection& connection,
           const runtime::CancellationToken& cancel) -> std::expected<std::uint32_t, SqlError> {
            if (!connection.is_open()) {
                return not_open("step");
            }
            return read_user_version(connection, cancel);
        },
        std::move(completion));
}

} // namespace tessera::appfw::sql
