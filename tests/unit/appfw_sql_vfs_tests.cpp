// SPDX-License-Identifier: MIT
//
// Fault-injecting VFS cases for the opt-in SQLite module (issue #983). The AF7 fake FileSystem
// cannot reach SQLite's file I/O, which goes through its own VFS, so this registers a shim VFS over
// the platform default and makes it fail on demand: a full disk (write), a failed sync and a failed
// journal delete. SQLite never renames its database; deleting the rollback journal is the final
// step of a commit and the nearest failure to a failed rename. Each case must surface a typed
// error, leave the data consistent and leave the connection usable once the fault clears.

#include "tessera/appfw/sql.hpp"
#include "tessera/runtime/manual_io_executor.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <utility>

namespace {

using namespace tessera::appfw::sql;

int g_failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_sql_vfs: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

enum class Fault { None, Write, Sync, Delete };
Fault g_fault = Fault::None;

sqlite3_vfs* g_real = nullptr;
sqlite3_vfs g_shim;

struct ShimFile {
    sqlite3_file base;
    sqlite3_file* real;
};

ShimFile* shim(sqlite3_file* file) {
    return reinterpret_cast<ShimFile*>(file);
}

int shim_close(sqlite3_file* f) {
    const int rc = shim(f)->real->pMethods->xClose(shim(f)->real);
    f->pMethods = nullptr;
    return rc;
}
int shim_read(sqlite3_file* f, void* buf, int n, sqlite3_int64 off) {
    return shim(f)->real->pMethods->xRead(shim(f)->real, buf, n, off);
}
int shim_write(sqlite3_file* f, const void* buf, int n, sqlite3_int64 off) {
    if (g_fault == Fault::Write) {
        return SQLITE_FULL;
    }
    return shim(f)->real->pMethods->xWrite(shim(f)->real, buf, n, off);
}
int shim_truncate(sqlite3_file* f, sqlite3_int64 size) {
    return shim(f)->real->pMethods->xTruncate(shim(f)->real, size);
}
int shim_sync(sqlite3_file* f, int flags) {
    if (g_fault == Fault::Sync) {
        return SQLITE_IOERR_FSYNC;
    }
    return shim(f)->real->pMethods->xSync(shim(f)->real, flags);
}
int shim_size(sqlite3_file* f, sqlite3_int64* size) {
    return shim(f)->real->pMethods->xFileSize(shim(f)->real, size);
}
int shim_lock(sqlite3_file* f, int level) {
    return shim(f)->real->pMethods->xLock(shim(f)->real, level);
}
int shim_unlock(sqlite3_file* f, int level) {
    return shim(f)->real->pMethods->xUnlock(shim(f)->real, level);
}
int shim_reserved(sqlite3_file* f, int* out) {
    return shim(f)->real->pMethods->xCheckReservedLock(shim(f)->real, out);
}
int shim_control(sqlite3_file* f, int op, void* arg) {
    return shim(f)->real->pMethods->xFileControl(shim(f)->real, op, arg);
}
int shim_sector(sqlite3_file* f) {
    return shim(f)->real->pMethods->xSectorSize(shim(f)->real);
}
int shim_chars(sqlite3_file* f) {
    return shim(f)->real->pMethods->xDeviceCharacteristics(shim(f)->real);
}

// Version 1: no shared-memory methods, so the shim is for rollback-journal databases only.
sqlite3_io_methods make_methods() {
    sqlite3_io_methods m{};
    m.iVersion = 1;
    m.xClose = shim_close;
    m.xRead = shim_read;
    m.xWrite = shim_write;
    m.xTruncate = shim_truncate;
    m.xSync = shim_sync;
    m.xFileSize = shim_size;
    m.xLock = shim_lock;
    m.xUnlock = shim_unlock;
    m.xCheckReservedLock = shim_reserved;
    m.xFileControl = shim_control;
    m.xSectorSize = shim_sector;
    m.xDeviceCharacteristics = shim_chars;
    return m;
}
const sqlite3_io_methods g_methods = make_methods();

int shim_open(sqlite3_vfs*, const char* name, sqlite3_file* file, int flags, int* out_flags) {
    ShimFile* self = shim(file);
    self->real = reinterpret_cast<sqlite3_file*>(self + 1);
    const int rc = g_real->xOpen(g_real, name, self->real, flags, out_flags);
    file->pMethods = rc == SQLITE_OK ? &g_methods : nullptr;
    return rc;
}

int shim_delete(sqlite3_vfs*, const char* name, int sync_dir) {
    if (g_fault == Fault::Delete) {
        return SQLITE_IOERR_DELETE;
    }
    return g_real->xDelete(g_real, name, sync_dir);
}

void register_shim() {
    g_real = sqlite3_vfs_find(nullptr);
    g_shim = *g_real;
    g_shim.zName = "tessera-fault";
    g_shim.szOsFile = static_cast<int>(sizeof(ShimFile)) + g_real->szOsFile;
    g_shim.xOpen = shim_open;
    g_shim.xDelete = shim_delete;
    sqlite3_vfs_register(&g_shim, 0);
}

template<typename T, typename Start>
std::expected<T, SqlError> run(tessera::runtime::ManualIoExecutor& executor, Start&& start) {
    std::optional<std::expected<T, SqlError>> out;
    expect(start([&](std::expected<T, SqlError> result) { out = std::move(result); }), "posted");
    executor.run(1);
    executor.run_completions(1);
    if (!out) {
        return std::unexpected(SqlError{SqlErrorCode::Sqlite, "test", 0, "no completion"});
    }
    return std::move(*out);
}

std::expected<ExecResult, SqlError> exec(tessera::runtime::ManualIoExecutor& executor, Database& db,
                                         std::string sql) {
    return run<ExecResult>(executor,
                           [&](auto done) { return db.execute(std::move(sql), {}, {}, done); });
}

std::int64_t row_count(tessera::runtime::ManualIoExecutor& executor, Database& db) {
    auto rows = run<Rows>(
        executor, [&](auto done) { return db.query("SELECT count(*) FROM kv", {}, {}, done); });
    return rows && rows->rows.size() == 1 ? rows->rows[0][0].as_int64().value_or(-1) : -1;
}

// The extended result code's primary part, the stable thing to assert on.
int primary(const SqlError& error) {
    return error.native & 0xff;
}

void faults_are_typed_and_leave_the_database_usable() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(TESSERA_SQL_SCRATCH_DIR) / "vfs";
    fs::remove_all(dir);
    fs::create_directories(dir);
    tessera::runtime::ManualIoExecutor executor;
    Database db(executor);
    OpenOptions options;
    options.wal = false;
    options.vfs = "tessera-fault";
    auto opened = run<std::monostate>(executor, [&](auto done) {
        return db.open((dir / "fault.db").string(), options, {},
                       [done](std::expected<void, SqlError> r) {
                           done(r.transform([] { return std::monostate{}; }));
                       });
    });
    if (!expect(opened.has_value(), "opens through the shim VFS")) {
        return;
    }
    expect(exec(executor, db, "CREATE TABLE kv(k INTEGER PRIMARY KEY, v TEXT)").has_value(),
           "create");
    expect(exec(executor, db, "INSERT INTO kv(v) VALUES('base')").has_value(), "baseline row");
    expect(row_count(executor, db) == 1, "one baseline row");

    g_fault = Fault::Write;
    auto full = exec(executor, db, "INSERT INTO kv(v) VALUES('full')");
    expect(!full && primary(full.error()) == SQLITE_FULL, "a full disk is the engine's FULL error");
    g_fault = Fault::None;
    expect(row_count(executor, db) == 1, "the failed write left the baseline");
    expect(exec(executor, db, "INSERT INTO kv(v) VALUES('after-full')").has_value(),
           "writes work again once space returns");

    g_fault = Fault::Sync;
    auto sync = exec(executor, db, "INSERT INTO kv(v) VALUES('sync')");
    expect(!sync && primary(sync.error()) == SQLITE_IOERR, "a failed sync is an I/O error");
    g_fault = Fault::None;
    const std::int64_t after_sync = row_count(executor, db);
    expect(after_sync == 2, "the unsynced commit did not survive");
    expect(exec(executor, db, "INSERT INTO kv(v) VALUES('after-sync')").has_value(),
           "writes work again after a sync failure");

    g_fault = Fault::Delete;
    auto removed = exec(executor, db, "INSERT INTO kv(v) VALUES('delete')");
    expect(!removed && primary(removed.error()) == SQLITE_IOERR,
           "a failed journal delete (the commit's last step) is an I/O error");
    g_fault = Fault::None;
    const std::int64_t after_delete = row_count(executor, db);
    expect(after_delete == 3 || after_delete == 4,
           "the table is consistent after a delete failure");
    expect(exec(executor, db, "INSERT INTO kv(v) VALUES('last')").has_value(),
           "writes work again after a delete failure");
    fs::remove_all(dir);
}

} // namespace

int main() {
    register_shim();
    faults_are_typed_and_leave_the_database_usable();
    if (g_failures != 0) {
        std::cerr << "appfw_sql_vfs: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_sql_vfs: ok\n";
    return 0;
}
