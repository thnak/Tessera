// SPDX-License-Identifier: MIT
//
// Contract suite for the opt-in SQLite module (issue #837). It runs against the real engine on
// `:memory:`, which is deterministic and fast, so no fake engine is needed. The executor is the
// manual one (bodies and completions are pumped by hand), except in the two cases that exist to
// prove a real thread is involved. File cases use a scratch directory under the build tree.

#include "tessera/appfw/sql.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/manual_io_executor.hpp"
#include "tessera/runtime/worker_io_executor.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace tessera::appfw::sql;
using tessera::runtime::CancellationSource;
using tessera::runtime::CancellationToken;
using tessera::runtime::CompletionQueue;
using tessera::runtime::ManualIoExecutor;
using tessera::runtime::WorkerIoExecutor;

int g_failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_sql: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// Posts one operation on the manual executor, pumps its body and completion, returns the result.
template<typename T, typename Start>
std::expected<T, SqlError> run(ManualIoExecutor& executor, Start&& start) {
    std::optional<std::expected<T, SqlError>> out;
    const bool posted = start([&](std::expected<T, SqlError> result) { out = std::move(result); });
    expect(posted, "the operation is posted");
    executor.run(1);
    executor.run_completions(1);
    if (!out) {
        return std::unexpected(SqlError{SqlErrorCode::Sqlite, "test", 0, "no completion"});
    }
    return std::move(*out);
}

struct Fixture {
    ManualIoExecutor executor;
    Database db{executor};
    CancellationToken none;

    explicit Fixture(std::string path = ":memory:", OpenOptions options = {}) {
        auto opened = run<std::monostate>(executor, [&](auto done) {
            return db.open(std::move(path), options, none, [done](std::expected<void, SqlError> r) {
                done(r.transform([] { return std::monostate{}; }));
            });
        });
        expect(opened.has_value(), "the database opens");
    }

    std::expected<ExecResult, SqlError> exec(std::string sql, Binds binds = {}) {
        return run<ExecResult>(executor, [&](auto done) {
            return db.execute(std::move(sql), std::move(binds), none, done);
        });
    }
    std::expected<Rows, SqlError> query(std::string sql, Binds binds = {},
                                        CancellationToken token = {}) {
        return run<Rows>(executor, [&](auto done) {
            return db.query(std::move(sql), std::move(binds), token, done);
        });
    }
    std::expected<std::uint32_t, SqlError> version() {
        return run<std::uint32_t>(executor, [&](auto done) { return db.user_version(none, done); });
    }
    std::expected<std::uint32_t, SqlError> migrate(std::vector<Migration> steps) {
        return run<std::uint32_t>(
            executor, [&](auto done) { return db.migrate(std::move(steps), none, done); });
    }
    std::expected<void, SqlError> transaction(Database::TransactionBody body,
                                              CancellationToken token = {}) {
        return run<std::monostate>(executor,
                                   [&](auto done) {
                                       return db.transaction(
                                           std::move(body), token,
                                           [done](std::expected<void, SqlError> r) {
                                               done(r.transform([] { return std::monostate{}; }));
                                           });
                                   })
            .transform([](std::monostate) {});
    }
};

std::int64_t count_of(Fixture& f, const std::string& table) {
    auto rows = f.query("SELECT count(*) FROM " + table);
    return rows && rows->rows.size() == 1 ? rows->rows[0][0].as_int64().value_or(-1) : -1;
}

void typed_values_round_trip_without_conversion() {
    Fixture f;
    expect(f.exec("CREATE TABLE t(i INTEGER, d REAL, s TEXT, b BLOB, n)").has_value(), "create");
    const Blob blob{0, 1, 2, 255};
    auto inserted =
        f.exec("INSERT INTO t VALUES(?,?,?,?,?)",
               {Value(std::int64_t{-42}), Value(2.5), Value("h\xC3\xA9llo"), Value(blob), Value()});
    expect(inserted && inserted->changes == 1 && inserted->last_insert_rowid == 1, "insert result");

    auto rows = f.query("SELECT i, d, s, b, n FROM t");
    if (!expect(rows && rows->rows.size() == 1, "one row")) {
        return;
    }
    expect(rows->columns == std::vector<std::string>({"i", "d", "s", "b", "n"}), "column names");
    const Row& row = rows->rows[0];
    expect(row[0].as_int64() == std::int64_t{-42}, "int64 round trips");
    expect(row[1].as_double() == 2.5, "double round trips");
    expect(row[2].as_string() == std::string_view("h\xC3\xA9llo"), "text round trips");
    const auto got_blob = row[3].as_blob();
    expect(got_blob && std::equal(got_blob->begin(), got_blob->end(), blob.begin(), blob.end()),
           "blob round trips");
    expect(row[4].is_null(), "null round trips");

    // Typed errors, never a silent conversion.
    expect(row[0].as_double().error().code == SqlErrorCode::TypeMismatch, "int is not a double");
    expect(row[1].as_int64().error().code == SqlErrorCode::TypeMismatch, "double is not an int");
    expect(row[2].as_blob().error().code == SqlErrorCode::TypeMismatch, "text is not a blob");
    expect(row[4].as_string().error().code == SqlErrorCode::TypeMismatch, "null is not text");

    // An empty blob and an empty string stay distinct from NULL.
    expect(f.exec("INSERT INTO t(b, s) VALUES(?, ?)", {Value(Blob{}), Value("")}).has_value(),
           "empty values bind");
    auto empties = f.query("SELECT s, b FROM t WHERE i IS NULL");
    expect(empties && empties->rows.size() == 1 && empties->rows[0][0].is_string() &&
               empties->rows[0][1].is_blob(),
           "empty text and blob are not null");
}

void argument_errors_are_typed() {
    Fixture f;
    expect(f.exec("CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT NOT NULL)").has_value(), "create");
    expect(f.exec("INSERT INTO t VALUES(?, ?)", {Value(1)}).error().code == SqlErrorCode::BindCount,
           "too few binds");
    expect(f.exec("SELECT 1", {Value(1)}).error().code == SqlErrorCode::BindCount,
           "too many binds");
    expect(f.exec("SELECT 1; SELECT 2").error().code == SqlErrorCode::InvalidArgument,
           "two statements are rejected");
    expect(f.exec("   ").error().code == SqlErrorCode::InvalidArgument, "empty SQL is rejected");
    expect(f.exec("SELEC 1").error().code == SqlErrorCode::Sqlite,
           "a syntax error is an engine error");
    expect(f.exec("INSERT INTO t VALUES(1, 'x')").has_value(), "first insert");
    expect(f.exec("INSERT INTO t VALUES(1, 'y')").error().code == SqlErrorCode::Constraint,
           "a primary-key clash is a Constraint error");
    expect(f.exec("INSERT INTO t VALUES(2, NULL)").error().code == SqlErrorCode::Constraint,
           "NOT NULL is a Constraint error");
    // A statement that failed leaves the cached handle reusable.
    expect(f.exec("INSERT INTO t VALUES(?, ?)", {Value(3), Value("z")}).has_value(),
           "the connection is usable after an error");

    OpenOptions small;
    small.max_rows = 2;
    Fixture capped(":memory:", small);
    auto rows = capped.query(
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<5) SELECT x FROM c");
    expect(!rows && rows.error().code == SqlErrorCode::TooLarge, "max_rows caps a result");
}

void operations_before_open_and_double_open_fail() {
    ManualIoExecutor executor;
    Database db(executor);
    auto unopened =
        run<Rows>(executor, [&](auto done) { return db.query("SELECT 1", {}, {}, done); });
    expect(!unopened && unopened.error().code == SqlErrorCode::NotOpen, "query before open");

    Fixture f;
    auto again = run<std::monostate>(f.executor, [&](auto done) {
        return f.db.open(":memory:", {}, {}, [done](std::expected<void, SqlError> r) {
            done(r.transform([] { return std::monostate{}; }));
        });
    });
    expect(!again && again.error().code == SqlErrorCode::AlreadyOpen, "second open");

    auto closed = run<std::monostate>(f.executor, [&](auto done) {
        return f.db.close([done](std::expected<void, SqlError> r) {
            done(r.transform([] { return std::monostate{}; }));
        });
    });
    expect(closed.has_value(), "close");
    expect(!f.query("SELECT 1") && f.query("SELECT 1").error().code == SqlErrorCode::NotOpen,
           "query after close");
}

void transactions_commit_or_roll_back() {
    Fixture f;
    expect(f.exec("CREATE TABLE t(a INTEGER)").has_value(), "create");

    auto committed = f.transaction([](Transaction& tx) -> std::expected<void, SqlError> {
        for (int i = 0; i < 3; ++i) {
            if (auto r = tx.execute("INSERT INTO t VALUES(?)", {Value(i)}); !r) {
                return std::unexpected(r.error());
            }
        }
        auto inside = tx.query("SELECT count(*) FROM t");
        if (!inside || inside->rows[0][0].as_int64() != std::int64_t{3}) {
            return std::unexpected(SqlError{SqlErrorCode::Sqlite, "test", 0, "not visible inside"});
        }
        return {};
    });
    expect(committed.has_value() && count_of(f, "t") == 3, "a successful body commits");

    auto failed = f.transaction([](Transaction& tx) -> std::expected<void, SqlError> {
        (void)tx.execute("INSERT INTO t VALUES(99)");
        return std::unexpected(SqlError{SqlErrorCode::InvalidArgument, "body", 0, "no"});
    });
    expect(!failed && failed.error().code == SqlErrorCode::InvalidArgument,
           "the body's error returns");
    expect(count_of(f, "t") == 3, "a failing body rolls back");

    auto engine_error = f.transaction([](Transaction& tx) -> std::expected<void, SqlError> {
        (void)tx.execute("INSERT INTO t VALUES(100)");
        auto bad = tx.execute("INSERT INTO nope VALUES(1)");
        return bad ? std::expected<void, SqlError>{} : std::unexpected(bad.error());
    });
    expect(!engine_error && count_of(f, "t") == 3, "an engine error inside the body rolls back");
}

void migrations_apply_in_order_and_roll_back_a_failed_step() {
    Fixture f;
    expect(f.version() == std::uint32_t{0}, "a new database is version 0");

    std::vector<Migration> steps{
        {1, "CREATE TABLE a(x INTEGER); INSERT INTO a VALUES(1);"},
        {2, "CREATE TABLE b(y TEXT); INSERT INTO b VALUES('two');"},
    };
    expect(f.migrate(steps) == std::uint32_t{2}, "v0 -> v2 applies both");
    expect(count_of(f, "a") == 1 && count_of(f, "b") == 1, "both steps ran");
    expect(f.version() == std::uint32_t{2}, "user_version records the last step");
    expect(f.migrate(steps) == std::uint32_t{2}, "re-running is a no-op");
    expect(count_of(f, "a") == 1, "an applied step does not run again");

    // v2 -> v4: step 3 is fine, step 4 fails half way. Step 4 rolls back entirely (its CREATE
    // included) and user_version stays at 3.
    steps.push_back({3, "CREATE TABLE c(z INTEGER);"});
    steps.push_back({4, "CREATE TABLE d(w INTEGER); INSERT INTO missing VALUES(1);"});
    auto failed = f.migrate(steps);
    expect(!failed && failed.error().code == SqlErrorCode::Migration,
           "a failed step is a Migration error");
    expect(failed.error().message.starts_with("migration 4:"), "the error names the step");
    expect(f.version() == std::uint32_t{3}, "user_version stays at the last good step");
    expect(!f.query("SELECT * FROM d").has_value(), "the failed step left no table behind");
    expect(f.query("SELECT * FROM c").has_value(), "the earlier good step is kept");

    expect(f.migrate({{2, "SELECT 1"}, {2, "SELECT 1"}}).error().code ==
               SqlErrorCode::InvalidArgument,
           "versions must increase");
    expect(f.migrate({{0, "SELECT 1"}}).error().code == SqlErrorCode::InvalidArgument,
           "version 0 is not a migration");
    expect(f.migrate({{1, "SELECT 1"}, {2, "SELECT 1"}}).error().code == SqlErrorCode::Migration,
           "a database newer than the code is refused");
}

void a_cancelled_token_does_nothing() {
    Fixture f;
    expect(f.exec("CREATE TABLE t(a INTEGER)").has_value(), "create");
    CancellationSource source;
    source.cancel();
    auto result = run<ExecResult>(f.executor, [&](auto done) {
        return f.db.execute("INSERT INTO t VALUES(1)", {}, source.token(), done);
    });
    expect(!result && result.error().code == SqlErrorCode::Cancelled, "cancelled before the body");
    expect(count_of(f, "t") == 0, "nothing was written");
}

void cancelling_inside_a_transaction_interrupts_the_statement_and_rolls_back() {
    Fixture f;
    expect(f.exec("CREATE TABLE t(a INTEGER)").has_value(), "create");
    CancellationSource source;
    std::promise<void> started;
    std::optional<std::expected<void, SqlError>> outcome;

    const bool posted = f.db.transaction(
        [&](Transaction& tx) -> std::expected<void, SqlError> {
            (void)tx.execute("INSERT INTO t VALUES(1)");
            started.set_value();
            // Never finishes on its own: only the cancellation poll can end it.
            auto endless = tx.query("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
                                    "WHERE x < 1000000000000) SELECT count(*) FROM c");
            return endless ? std::expected<void, SqlError>{} : std::unexpected(endless.error());
        },
        source.token(), [&](std::expected<void, SqlError> r) { outcome = std::move(r); });
    expect(posted, "posted");

    std::thread worker([&] { f.executor.run(1); });
    started.get_future().wait();
    source.cancel();
    worker.join();
    f.executor.run_completions(1);

    expect(outcome && !*outcome && outcome->error().code == SqlErrorCode::Cancelled,
           "the long statement is interrupted and reported as Cancelled");
    expect(count_of(f, "t") == 0, "the interrupted transaction rolled back");
    expect(f.exec("INSERT INTO t VALUES(2)").has_value(), "the connection is usable afterwards");
}

void posting_never_runs_the_body_on_the_calling_thread() {
    Fixture f;
    bool done = false;
    const bool posted = f.db.query("SELECT 1", {}, {}, [&](auto) { done = true; });
    expect(posted && !done && f.executor.pending_work() == 1,
           "post only enqueues: the caller does not block on the engine");
    f.executor.run(1);
    expect(!done, "the completion waits for the drain");
    f.executor.run_completions(1);
    expect(done, "the completion runs on the draining thread");

    ManualIoExecutor tiny(1);
    Database db(tiny);
    expect(db.query("SELECT 1", {}, {}, {}), "first fits");
    bool refused_ran = false;
    expect(!db.query("SELECT 1", {}, {}, [&](auto) { refused_ran = true; }),
           "a full executor refuses the submission");
    tiny.clear();
    expect(!refused_ran, "a refused submission never completes");
}

[[maybe_unused]] void a_real_executor_keeps_the_caller_free_while_a_query_runs() {
    CompletionQueue queue(64);
    WorkerIoExecutor executor(queue, WorkerIoExecutor::Config{2, 2, 64});
    Database db(executor);
    std::atomic<bool> opened{false};
    std::atomic<bool> finished{false};
    std::optional<std::expected<Rows, SqlError>> result;

    auto pump_until = [&](auto&& predicate) {
        for (int i = 0; i < 10'000 && !predicate(); ++i) {
            queue.run(16);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return predicate();
    };
    db.open(":memory:", {}, {}, [&](std::expected<void, SqlError> r) { opened = r.has_value(); });
    expect(pump_until([&] { return opened.load(); }), "opens through a real executor");

    CancellationSource source;
    db.query(
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 1000000000000) "
        "SELECT count(*) FROM c",
        {}, source.token(), [&](std::expected<Rows, SqlError> r) {
            result = std::move(r);
            finished = true;
        });
    // The calling thread keeps ticking (draining its queue) while the engine grinds.
    for (int tick = 0; tick < 20; ++tick) {
        queue.run(16);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(!finished.load(), "the long query has not finished while the caller ticked");
    source.cancel();
    expect(pump_until([&] { return finished.load(); }), "cancellation ends the query");
    expect(result && !*result && result->error().code == SqlErrorCode::Cancelled,
           "reported Cancelled");
    executor.shutdown();
}

[[maybe_unused]] void a_file_database_persists_and_a_corrupt_file_is_a_typed_error() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(TESSERA_SQL_SCRATCH_DIR);
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "app.db").string();
    {
        Fixture f(path);
        expect(f.migrate({{1, "CREATE TABLE kv(k TEXT PRIMARY KEY, v BLOB);"}}) == std::uint32_t{1},
               "migrate a file database");
        expect(f.exec("INSERT INTO kv VALUES('a', ?)", {Value(Blob{7, 8})}).has_value(), "insert");
        auto closed = run<std::monostate>(f.executor, [&](auto done) {
            return f.db.close([done](std::expected<void, SqlError> r) {
                done(r.transform([] { return std::monostate{}; }));
            });
        });
        expect(closed.has_value(), "close");
    }
    {
        Fixture f(path);
        expect(f.version() == std::uint32_t{1}, "user_version survives a reopen");
        auto rows = f.query("SELECT v FROM kv WHERE k = ?", {Value("a")});
        expect(rows && rows->rows.size() == 1 && rows->rows[0][0].is_blob(), "the row survives");
    }

    const std::string bad = (dir / "bad.db").string();
    {
        std::ofstream out(bad, std::ios::binary);
        out << std::string(4096, 'x');
    }
    ManualIoExecutor executor;
    Database db(executor);
    auto opened = run<std::monostate>(executor, [&](auto done) {
        return db.open(bad, {}, {}, [done](std::expected<void, SqlError> r) {
            done(r.transform([] { return std::monostate{}; }));
        });
    });
    expect(!opened && opened.error().code == SqlErrorCode::Sqlite, "a corrupt file fails to open");
    expect(!opened.error().message.empty(), "the engine message is kept");
    auto after = run<Rows>(executor, [&](auto done) { return db.query("SELECT 1", {}, {}, done); });
    expect(!after && after.error().code == SqlErrorCode::NotOpen, "a failed open leaves it closed");
    fs::remove_all(dir);
}

void memory_only_refuses_a_file_database() {
    ManualIoExecutor executor;
    Database db(executor);
    OpenOptions options;
    options.memory_only = true;
    auto file = run<std::monostate>(executor, [&](auto done) {
        return db.open("never-created.db", options, {}, [done](std::expected<void, SqlError> r) {
            done(r.transform([] { return std::monostate{}; }));
        });
    });
    expect(!file && file.error().code == SqlErrorCode::Unsupported, "a file path is Unsupported");
    expect(!std::filesystem::exists("never-created.db"), "nothing was created");
    auto memory = run<std::monostate>(executor, [&](auto done) {
        return db.open(":memory:", options, {}, [done](std::expected<void, SqlError> r) {
            done(r.transform([] { return std::monostate{}; }));
        });
    });
    expect(memory.has_value(), "`:memory:` still opens");
}

} // namespace

int main() {
    memory_only_refuses_a_file_database();
    typed_values_round_trip_without_conversion();
    argument_errors_are_typed();
    operations_before_open_and_double_open_fail();
    transactions_commit_or_roll_back();
    migrations_apply_in_order_and_roll_back_a_failed_step();
    a_cancelled_token_does_nothing();
    cancelling_inside_a_transaction_interrupts_the_statement_and_rolls_back();
    posting_never_runs_the_body_on_the_calling_thread();
#if !defined(__EMSCRIPTEN__)
    // Wasm is in-memory only and single-threaded: no file database and no worker thread there.
    a_real_executor_keeps_the_caller_free_while_a_query_runs();
    a_file_database_persists_and_a_corrupt_file_is_a_typed_error();
#endif
    if (g_failures != 0) {
        std::cerr << "appfw_sql: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_sql: ok\n";
    return 0;
}
