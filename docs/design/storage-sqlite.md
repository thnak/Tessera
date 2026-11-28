# SQLite adapter: the opt-in `tessera_appfw_storage_sqlite` module (issue #837)

Status: implemented as a first landing (module, contract suite, build switch). The decision is the
owner's answer to [open question 4](app-framework-open-questions.md#4-sqlite-adapter-in-tree-or-community-only)
(accepted 2026-11-12, option A). Code: `include/tessera/appfw/sql.hpp`, `src/appfw/sqlite/`, tests
`tests/unit/appfw_sql_tests.cpp` (ctest `appfw_sql`).

## 1. Switch and layering

- Off by default: `-DTESSERA_APPFW_SQLITE=ON` adds the `tessera_appfw_storage_sqlite` static library
  and its test. With the option off nothing is downloaded, compiled or linked, and no default target
  depends on it. The `tessera.json` module switch is not parsed yet (the manifest loader reads no `dependencies`
  block at all, so a key would be dead weight); the CMake option is the V1
  switch ([project-config-v1](../specs/project-config-v1.md), `dependencies`).
- The engine is the pinned amalgamation 3.54.1, fetched at configure time by SHA-256 (the FreeType
  precedent) rather than committing a 9 MB generated file; an offline machine points
  `TESSERA_SQLITE_SOURCE_DIR` at an unpacked copy. It is built as `tessera_sqlite3`, linked
  PRIVATE, so no SQLite header reaches a consumer. Compiled out: extension loading, shared cache,
  deprecated APIs, double-quoted string literals.
- Layering: the module links `tessera_appfw_storage` and `tessera_runtime`. The key/value and
  document stores never depend on it.

## 2. Surface

`tessera::appfw::sql::Database` follows the `Storage` conventions (callbacks, the submit contract,
`std::expected` errors) rather than the `runtime::Task` sketch in the issue, so one module family
behaves one way.

| Call | Does |
|---|---|
| `open(path, OpenOptions, token, cb)` | file path or `:memory:`; WAL and foreign keys on by default |
| `execute` / `query` | one statement with typed binds; prepared once per connection, re-bound per run (cache of 32) |
| `transaction(body, token, cb)` | `BEGIN IMMEDIATE`; body runs on the executor; commit, or roll back on error or cancellation |
| `migrate(steps, token, cb)` | by `PRAGMA user_version`, one transaction per step |
| `user_version`, `close` | |

Values are null, int64, double, string or blob. Binding does not convert; reading a column as another
type is `SqlErrorCode::TypeMismatch`. A bind-count mismatch, a multi-statement string, a result over
`max_rows` and constraint violations are each their own error code.

## 3. Threading and cancellation

Every call only posts to the `IoTaskExecutor`; the connection is touched in the work body, under the
database's own mutex (one connection, one writer, serialized even on a multi-worker executor).
Completions run where the queue is drained. Cancellation is polled from SQLite's progress handler
every 1000 VM instructions and aborts the statement, which SQLite reports as an interrupt. This is
the effect of `sqlite3_interrupt` without its lost-wakeup case (an interrupt raised between two
statements is dropped by SQLite). A transaction in flight is rolled back; the connection stays usable.

## 4. Migrations

Versions are positive and strictly increasing. Steps newer than `user_version` run in order, each in
its own transaction together with the `user_version` update, so a failing step rolls back entirely
and the version stays at the last good step (error `Migration`, message `migration N: ...`). A
database whose version is newer than the newest migration is refused rather than guessed at.

## 5. Evidence

- Tests (all against the real engine; `:memory:` except the file case): typed round trips and
  mismatches, argument errors, not-open/double-open, commit and rollback, v0 -> v2 migration and a
  failed step, cancel-before-start, cancel of an endless statement inside a transaction (rolled
  back), post-never-runs-inline plus refusal, a `WorkerIoExecutor` case where the caller keeps ticking
  while a long query runs and then cancels it, and a file database that persists across reopen plus a
  corrupt file that fails to open as a typed error.
- Binary size (MSVC Release, vs2026): `appfw_sql_tests.exe` is 1.56 MB against 0.50 MB for
  `appfw_storage_tests.exe`, so the module and engine add about 1.0 MB to a small static
  executable. The "about half a megabyte" estimate in the open-questions note was low for MSVC
  with default size flags.
- GCC 15 / Linux (thnak-server, Release, `-Werror`): configures, builds and `appfw_sql` passes. Size
  of `appfw_sql_tests` against `appfw_storage_tests` (both link the whole runtime): `MinSizeRel`
  (`-Os`) 2.77 MB vs 1.93 MB (+0.85 MB, +0.74 MB stripped); `-Os` plus LTO 2.72 MB vs 1.87 MB
  (+0.85 MB, +0.73 MB stripped). LTO barely helps because the engine is one translation unit with
  few cross-unit calls; the engine itself is about 0.7 MB, so the 0.5 MB estimate stays low.
- CI: `.github/workflows/appfw-sqlite.yml` builds and runs only `appfw_sql_tests` with
  `-DTESSERA_APPFW_SQLITE=ON` on GCC, path-filtered to `src/appfw/**` and the public appfw headers.

## 6. Wasm, VFS faults and what is left

- Wasm is in-memory only (#983). Under Emscripten the engine is built single-threaded with no WAL and
  in-memory temp storage, and `OpenOptions::memory_only` is forced on: any path but `:memory:` is
  refused with `SqlErrorCode::Unsupported` (settable on native, so the refusal is tested there).
  Verified with emsdk 7.0.0 on the Linux box: the module builds and `appfw_sql_tests` passes under
  Node (file, worker-thread and thread-based cancel cases are compiled out there).
- Fault-injecting VFS (`tests/unit/appfw_sql_vfs_tests.cpp`, `OpenOptions::vfs`): a shim over the
  platform VFS fails a write (full disk: `SQLITE_FULL`), a sync and the journal delete. SQLite never
  renames its database; the journal delete is the last step of a commit and the nearest analogue of a
  failed rename. Each case returns a typed error, leaves a consistent table and the connection
  writable once the fault clears. The AF7 fake `FileSystem` cannot reach these: SQLite opens files
  through its own VFS.
- The `tessera.json` `dependencies.modules` key (`"sqlite"`) is read and validated by the codegen
  loader; wiring it to the generated CMake target is not done (the build still uses
  `TESSERA_APPFW_SQLITE`).
- Left: a durable browser store (IndexedDB or OPFS VFS; the open risk in the questions note),
  concurrent readers (additive, only if measured) and the generated-CMake wiring above.
