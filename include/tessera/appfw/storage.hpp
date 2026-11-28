// SPDX-License-Identifier: MIT
//
// The storage module (ADR-050 section 8, row `storage`; issue #179).
//
// It is layer 1: the `tessera_appfw_storage` static library, depending on `tessera_appfw_core`, the
// runtime's I/O-executor seam and the HAL file-system seam. It owns *named project data* - settings
// (small key/value) and documents (app data). Every byte it reads or writes goes through an
// `IoTaskExecutor`, so a caller on the UI thread only posts work and later runs the completion.
//
// The properties that define the module:
//
//   1. **Atomic replace.** A write never truncates the target. It writes the complete new content
//   to
//      a temp file *in the target's own directory*, flushes it, and atomically renames it over the
//      target. A crash, a full disk, an I/O failure or a cancellation therefore leaves the complete
//      old content or the complete new content - never a partial file. The same-directory rule is
//      load-bearing: a rename across devices is a copy, and a copy is not atomic.
//   2. **Off the UI thread.** `load`/`save`/`remove`/`list` touch the file system only inside the
//      work body an `IoTaskExecutor` runs; the completion runs on the thread that drains the
//      completion queue (the UI thread in an app).
//   3. **Corruption fallback with a diagnostic.** A stored entry carries a header (magic, version,
//      kind, length, checksum). An entry whose header or checksum does not match is neither
//      returned nor deleted: the read reports `Corrupted`, records a bounded diagnostic, and the
//      caller falls back to its default.
//
// Cancellation: every operation takes a `CancellationToken`. Cancelling before the work body starts
// drops the work; cancelling while the body runs makes it stop at its next step boundary and delete
// its temp file. Either way the target keeps its old content and no `*.tmp` file survives.
//
// Submit contract (the same one `runtime::IoExecutor::submit` follows): a method returns `false`
// when it did not start the operation - an invalid name, or the executor refused the submission.
// In that case the completion is never invoked. When it returns `true`, the completion runs exactly
// once, with either a value or a `StorageError`.

#pragma once

#include "tessera/appfw/storage_migration.hpp"
#include "tessera/hal/filesystem.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/io_task_executor.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

// Why a storage operation failed. Cancellation is a value, not an exception (ADR-025), and a
// corrupted payload is distinct from a missing one so a caller can choose between "use the default"
// and "tell the user".
enum class StorageErrorCode : std::uint8_t {
    None = 0,
    NotFound,        // no entry under that name
    Corrupted,       // header/checksum mismatch, or an unsupported format version
    TooLarge,        // the stored or supplied payload exceeds the configured cap
    InvalidName,     // empty, absolute, or containing a separator / `..`
    Io,              // the file system reported a failure (see `native`)
    Cancelled,       // the caller cancelled the operation
    Busy,            // the executor refused the submission (at its bound)
    Unsupported,     // the durability/file-system guarantee is unavailable on this platform
    FutureVersion,   // stored by a newer build than this one; left untouched (migration)
    MigrationFailed, // the migration chain has a gap or a step rejected the data
};

[[nodiscard]] constexpr std::string_view storage_error_code_name(StorageErrorCode code) noexcept {
    switch (code) {
    case StorageErrorCode::None:
        return "none";
    case StorageErrorCode::NotFound:
        return "not found";
    case StorageErrorCode::Corrupted:
        return "corrupted";
    case StorageErrorCode::TooLarge:
        return "too large";
    case StorageErrorCode::InvalidName:
        return "invalid name";
    case StorageErrorCode::Io:
        return "io";
    case StorageErrorCode::Cancelled:
        return "cancelled";
    case StorageErrorCode::Busy:
        return "busy";
    case StorageErrorCode::Unsupported:
        return "unsupported";
    case StorageErrorCode::FutureVersion:
        return "future version";
    case StorageErrorCode::MigrationFailed:
        return "migration failed";
    }
    return "unknown";
}

struct StorageError {
    StorageErrorCode code = StorageErrorCode::Io;
    // The step that produced it ("validate", "read", "write-temp", "flush-temp", "replace",
    // "flush-dir", "submit"). The atomic sequence is named step by step, so a diagnostic says where
    // it broke instead of only that it broke.
    std::string_view step;
    std::int64_t native = 0;

    [[nodiscard]] bool operator==(const StorageError&) const = default;
};

// One recorded failure. Diagnostics are values and bounded (ADR-022): the store keeps the last N
// entries and a total count, never a log line per write.
struct StorageDiagnostic {
    StorageErrorCode code = StorageErrorCode::Io;
    std::string entry;
    std::string_view step;
    std::int64_t native = 0;
};

// Bounded diagnostics ring plus the counters a test and a diagnostics consumer read.
struct StorageDiagnostics {
    std::size_t recorded = 0;
    std::size_t dropped = 0;
    std::size_t capacity = 0;
    std::vector<StorageDiagnostic> entries;

    // Reads that found a bad header/checksum and reported `Corrupted`.
    std::uint64_t corruptions = 0;
    // Atomic replacements that completed.
    std::uint64_t writes = 0;
    // Writes abandoned by cancellation or a failure.
    std::uint64_t aborted_writes = 0;
    // Temp files removed by an aborted write (equal to `aborted_writes` when cleanup succeeded).
    std::uint64_t temp_files_removed = 0;
    // Entries upgraded by `load_migrated` and rewritten at the current version.
    std::uint64_t migrations = 0;
};

// A decoded document.
struct StoredValue {
    std::string data;
    std::uint32_t version = 0;
    // True when `load_migrated` upgraded the payload from an older stored version.
    bool migrated = false;
};

using Settings = std::vector<std::pair<std::string, std::string>>;

// How the module is wired. Everything is explicit (docs/design/services/common-contract.md
// section 1: "no environment-variable magic in production").
struct StorageConfig {
    // Root directory of the store. Entries live directly underneath it.
    std::string root;
    // Largest payload a single entry may hold. A larger read or write fails with `TooLarge`.
    std::size_t max_entry_bytes = 4 * 1024 * 1024;
    // Durability requested of every write (see `hal::Durability`).
    hal::Durability durability = hal::Durability::BestEffort;
    // Format version written into the envelope header. `load` of a newer version reports
    // `Corrupted` rather than guessing; `load_migrated` upgrades older ones and names a newer one.
    std::uint32_t format_version = 1;
    // How many failures the diagnostics ring keeps. 0 disables the ring but keeps the counters.
    std::size_t diagnostic_capacity = 16;
};

// What the atomic-replace sequence actually did. Returned through `atomic_replace` so a test can
// assert the ordering (temp written, flushed, replaced) instead of inferring it from the result.
struct AtomicWriteTrace {
    bool temp_written = false;
    bool temp_flushed = false;
    bool replaced = false;
    bool directory_flushed = false;
    // True when an aborted write removed its temp file. A successful write leaves this false: the
    // rename consumed the temp file rather than deleting it.
    bool temp_removed = false;
};

class Storage {
public:
    using ValueCallback = std::function<void(std::expected<StoredValue, StorageError>)>;
    using StatusCallback = std::function<void(std::expected<void, StorageError>)>;
    using ListCallback = std::function<void(std::expected<std::vector<std::string>, StorageError>)>;
    using SettingsCallback = std::function<void(std::expected<Settings, StorageError>)>;

    // `executor` runs the blocking half and delivers the completion on the draining thread, and
    // `files` is the platform seam. Both are borrowed and must outlive the store. There is no
    // completion-queue parameter: routing a completion to the UI thread is the executor's contract
    // (`IoTaskExecutor`), not a second thing the module has to be handed and could get wrong.
    Storage(StorageConfig config, runtime::IoTaskExecutor& executor, hal::FileSystem& files);
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // Reads a document. See the submit contract above.
    bool load(std::string name, runtime::CancellationToken token, ValueCallback completion);

    // Reads a document and upgrades it to `config().format_version` with `chain` (issue #411). The
    // chain runs inside the I/O body, after the checksum passed. On a successful upgrade the entry
    // is rewritten atomically at the current version, so the chain runs once per entry; a failed
    // rewrite is a diagnostic, not a failed load, because the caller already has usable data.
    // Outcomes: an older version loads migrated (`StoredValue::migrated`); a newer one reports
    // `FutureVersion` and the file is left untouched; a gap or a rejecting step reports
    // `MigrationFailed` and the file is left untouched. Returns false for a null chain too.
    bool load_migrated(std::string name, std::shared_ptr<const MigrationChain> chain,
                       runtime::CancellationToken token, ValueCallback completion);

    // Atomically replaces a document with `data`.
    bool save(std::string name, std::string data, runtime::CancellationToken token,
              StatusCallback completion);

    // Removes an entry. Removing an absent entry succeeds: a delete is idempotent, and a caller
    // that races two deletes should not see a failure for the one that lost.
    bool remove(std::string name, runtime::CancellationToken token, StatusCallback completion);

    // Names currently stored, sorted, with temp files filtered out.
    bool list(runtime::CancellationToken token, ListCallback completion);

    // Settings: an ordered (key, value) list in one entry, with its own envelope kind so it cannot
    // be confused with a raw document.
    bool save_settings(std::string name, Settings values, runtime::CancellationToken token,
                       StatusCallback completion);
    bool load_settings(std::string name, runtime::CancellationToken token,
                       SettingsCallback completion);

    [[nodiscard]] const StorageConfig& config() const noexcept;
    [[nodiscard]] StorageDiagnostics diagnostics() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// The raw atomic-replace sequence, exposed so a test can drive it step by step with a failing fake
// and so a caller that only needs "replace these bytes, atomically, off the UI thread" does not
// have to build a `Storage`. It is the exact code path every write entry point uses.
//
// Steps, in order (the order *is* the guarantee):
//   1. create the parent directory
//   2. write the complete payload to `<target>.<generation>.tmp` in the target's directory
//   3. flush the temp file
//   4. atomically replace the target with the temp file
//   5. `Durable` only: flush the directory
// A failure or cancellation after step 2 removes the temp file; the target keeps its previous
// content because step 4 never ran.
[[nodiscard]] std::expected<void, StorageError>
atomic_replace(hal::FileSystem& files, std::string_view target, std::string_view data,
               hal::Durability durability, const runtime::CancellationToken& token,
               std::uint64_t generation, AtomicWriteTrace* trace = nullptr);

// The temp path `atomic_replace` uses: same directory as `target`, so the rename is a same-device
// rename. Exposed for tests that assert that rule.
[[nodiscard]] std::string temp_path_for(std::string_view target, std::uint64_t generation);

// True when `name` is a legal entry name: non-empty, bounded, no path separator, no drive prefix,
// not `.`/`..` and not containing a `..` component. Rejects traversal from untrusted input
// (docs/design/services/storage.md section 4).
[[nodiscard]] bool is_valid_entry_name(std::string_view name) noexcept;

} // namespace tessera::appfw
