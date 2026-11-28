// SPDX-License-Identifier: MIT
//
// A bounded, evictable cache store (issue #411, item 2; ADR-050 section 8 "cache" class).
//
// A cache is the third storage class next to settings and documents: its entries can be recomputed,
// so the store is allowed to drop them. That permission is what a bound needs - `CacheStore` keeps
// the total size and the entry count under a configured limit by evicting the least recently used
// entry, and it is the only storage class that may delete data the caller did not ask it to.
//
//   * **Same write path.** Entries use the storage envelope and `atomic_replace`, on an
//     `IoTaskExecutor`, so a crash never leaves a torn cache file and the UI thread never does I/O.
//   * **Deterministic accounting.** "Recently used" is a logical tick the store increments on every
//     put and hit, not a wall-clock time, so eviction order is reproducible in a test and across
//     runs. Sizes are the on-disk (envelope-inclusive) bytes, so the bound is a bound on disk use.
//   * **Index in memory, truth on disk.** The index (name, size, tick) lives in the store; `scan`
//     rebuilds it from the directory after a restart (order by name, since ticks do not persist),
//     then enforces the limits. A corrupt entry found by `get` is deleted: unlike a document, a
//     cache entry is never the only copy.
//   * **The newest entry is never its own victim.** A put that does not fit alone (its envelope is
//     larger than `max_bytes` or `max_entry_bytes`) is refused at submit, not stored and evicted.
//
// Submit contract: identical to `Storage` - false means nothing started and the completion will not
// run; true means it runs exactly once.

#pragma once

#include "tessera/appfw/storage.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tessera::appfw {

struct CacheConfig {
    // Directory holding the cache entries, one file per name.
    std::string root;
    // Bound on the total on-disk bytes of all entries (envelope included).
    std::size_t max_bytes = 8 * 1024 * 1024;
    // Bound on the entry count.
    std::size_t max_entries = 1024;
    // Largest single entry (envelope included). Must not exceed `max_bytes` to be admitted.
    std::size_t max_entry_bytes = 1024 * 1024;
    hal::Durability durability = hal::Durability::BestEffort;
};

struct CacheStats {
    std::size_t bytes = 0;
    std::size_t entries = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t evicted_bytes = 0;
    // Evicted entries whose file could not be removed (left as an orphan until `scan` re-adopts
    // it).
    std::uint64_t evict_failures = 0;
    // Corrupt entries `get` found and deleted.
    std::uint64_t corrupt_dropped = 0;
};

class CacheStore {
public:
    using ValueCallback = Storage::ValueCallback;
    using StatusCallback = Storage::StatusCallback;

    // `executor` and `files` are borrowed and must outlive the store.
    CacheStore(CacheConfig config, runtime::IoTaskExecutor& executor, hal::FileSystem& files);
    ~CacheStore();

    CacheStore(const CacheStore&) = delete;
    CacheStore& operator=(const CacheStore&) = delete;

    // Stores `data` under `name` (replacing any previous entry), then evicts least-recently-used
    // entries until both bounds hold. False for an invalid name, a cancelled token, or an entry
    // that can never fit.
    bool put(std::string name, std::string data, runtime::CancellationToken token,
             StatusCallback completion);

    // Reads an entry and marks it most recently used. A miss is `NotFound`; a corrupt entry is
    // deleted and reported as `Corrupted`.
    bool get(std::string name, runtime::CancellationToken token, ValueCallback completion);

    // Removes an entry; absent is success.
    bool remove(std::string name, runtime::CancellationToken token, StatusCallback completion);

    // Rebuilds the index from the directory (restart path) and enforces the limits.
    bool scan(runtime::CancellationToken token, StatusCallback completion);

    [[nodiscard]] CacheStats stats() const;
    // Indexed names, least recently used first. For tests and diagnostics.
    [[nodiscard]] std::vector<std::string> names_by_recency() const;
    [[nodiscard]] const CacheConfig& config() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace tessera::appfw
