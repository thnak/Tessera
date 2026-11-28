// SPDX-License-Identifier: MIT
//
// Issue #411 item 2: the bounded cache. Accounting is asserted exactly: every entry's on-disk size
// is payload + the 24-byte envelope header, so the byte totals below are computed, not observed.
// "Recent" is a logical tick, so eviction order is the same on every run.

#include "appfw_storage_test_support.hpp"
#include "tessera/appfw/storage_cache.hpp"

#include <string>
#include <vector>

namespace storage_test {
namespace {

using tessera::appfw::CacheConfig;
using tessera::appfw::CacheStore;
using tessera::appfw::StoredValue;

constexpr std::size_t kHeader = 24;
constexpr std::size_t kPayload = 10;
constexpr std::size_t kEntry = kHeader + kPayload;

struct CacheHarness {
    explicit CacheHarness(std::size_t max_bytes, std::size_t max_entries = 100)
        : cache(CacheConfig{"/cache", max_bytes, max_entries, 4096, Durability::BestEffort},
                executor, files) {}

    void run() {
        executor.run(64);
        executor.run_completions(64);
    }
    bool put(const std::string& name, std::string data) {
        const bool posted = cache.put(name, std::move(data), {}, {});
        run();
        return posted;
    }
    std::expected<StoredValue, StorageError> get(const std::string& name) {
        std::expected<StoredValue, StorageError> out = std::unexpected(StorageError{});
        cache.get(name, {}, [&](auto v) { out = std::move(v); });
        run();
        return out;
    }

    ManualIoExecutor executor;
    FakeFileSystem files;
    CacheStore cache;
};

std::string payload(char fill) {
    return std::string(kPayload, fill);
}

void the_byte_bound_evicts_the_least_recently_used_entry() {
    CacheHarness h(3 * kEntry);
    h.put("a", payload('a'));
    h.put("b", payload('b'));
    h.put("c", payload('c'));
    expect(h.cache.stats().bytes == 3 * kEntry && h.cache.stats().entries == 3 &&
               h.cache.stats().evictions == 0,
           "three entries fit exactly; nothing is evicted at the bound");

    expect(h.get("a").has_value(), "a hit"); // a is now the most recent
    h.put("d", payload('d'));

    const auto stats = h.cache.stats();
    expect(stats.bytes == 3 * kEntry && stats.entries == 3, "the total stays at the bound");
    expect(stats.evictions == 1 && stats.evicted_bytes == kEntry, "exactly one entry was evicted");
    expect(!h.files.has_file("/cache/b"), "the least recently used entry (b) left the disk");
    expect(h.files.has_file("/cache/a") && h.files.has_file("/cache/c") &&
               h.files.has_file("/cache/d"),
           "the others stayed");
    expect((h.cache.names_by_recency() == std::vector<std::string>{"c", "a", "d"}),
           "recency order is c, a (touched), d (newest)");
    expect(h.files.temp_file_count() == 0, "no temp file survives");
}

void the_entry_bound_evicts_too() {
    CacheHarness h(100 * kEntry, 2);
    h.put("a", payload('a'));
    h.put("b", payload('b'));
    h.put("c", payload('c'));
    expect(h.cache.stats().entries == 2 && h.cache.stats().evictions == 1, "two entries kept");
    expect((h.cache.names_by_recency() == std::vector<std::string>{"b", "c"}), "a was evicted");
}

void a_big_put_evicts_several_and_never_itself() {
    CacheHarness h(3 * kEntry);
    h.put("a", payload('a'));
    h.put("b", payload('b'));
    h.put("c", payload('c'));
    h.put("big", std::string(2 * kEntry - kHeader, 'x')); // needs two slots' worth of room
    const auto stats = h.cache.stats();
    expect(stats.bytes <= 3 * kEntry, "the bound holds after a large put");
    expect(stats.evictions == 2, "two older entries made room");
    expect(h.files.has_file("/cache/big") && h.files.has_file("/cache/c"),
           "the new entry and the newest old entry survive");
}

void a_replacement_is_accounted_once_and_an_oversized_put_is_refused() {
    CacheHarness h(3 * kEntry);
    h.put("a", payload('a'));
    h.put("a", std::string(2 * kPayload, 'z'));
    expect(h.cache.stats().entries == 1 && h.cache.stats().bytes == kEntry + kPayload,
           "replacing an entry swaps its size instead of adding it");

    const std::size_t before = h.executor.pending_work();
    expect(!h.cache.put("huge", std::string(3 * kEntry, 'x'), {}, {}),
           "an entry that can never fit is refused at submit");
    expect(h.executor.pending_work() == before, "nothing was posted for it");
    expect(!h.cache.put("../x", payload('x'), {}, {}), "an invalid name is refused");
}

void a_miss_and_a_corrupt_entry_are_reported_and_dropped() {
    CacheHarness h(3 * kEntry);
    auto miss = h.get("nope");
    expect(!miss.has_value() && miss.error().code == StorageErrorCode::NotFound, "a miss");

    h.put("a", payload('a'));
    h.files.files["/cache/a"][kHeader] ^= 0x01; // flip a payload byte
    auto bad = h.get("a");
    expect(!bad.has_value() && bad.error().code == StorageErrorCode::Corrupted,
           "a flipped byte is Corrupted");
    expect(!h.files.has_file("/cache/a") && h.cache.stats().entries == 0 &&
               h.cache.stats().bytes == 0,
           "the corrupt entry was deleted and stops counting against the quota");
    const auto stats = h.cache.stats();
    expect(stats.misses == 2 && stats.hits == 0 && stats.corrupt_dropped == 1, "counters");
}

void remove_is_idempotent_and_releases_the_bytes() {
    CacheHarness h(3 * kEntry);
    h.put("a", payload('a'));
    std::expected<void, StorageError> result = std::unexpected(StorageError{});
    h.cache.remove("a", {}, [&](auto r) { result = std::move(r); });
    h.run();
    expect(result.has_value() && h.cache.stats().bytes == 0, "removed, bytes released");
    h.cache.remove("a", {}, [&](auto r) { result = std::move(r); });
    h.run();
    expect(result.has_value(), "removing an absent entry succeeds");
}

void a_failed_eviction_remove_is_counted_not_fatal() {
    CacheHarness h(2 * kEntry);
    h.put("a", payload('a'));
    h.put("b", payload('b'));
    h.files.fail = FailStep::Remove;
    expect(h.put("c", payload('c')), "the put itself is accepted");
    expect(h.cache.stats().evictions == 1 && h.cache.stats().evict_failures == 1,
           "the failed remove of the victim is counted");
    expect(h.cache.stats().bytes == 2 * kEntry, "the index still honours the bound");
}

void scan_rebuilds_the_index_after_a_restart_and_enforces_the_bound() {
    CacheHarness h(2 * kEntry);
    for (const char* name : {"a", "b", "c"}) {
        h.files.files[std::string("/cache/") + name] = std::string(kEntry, 'x');
    }
    h.files.files["/cache/old.0.tmp"] = "partial";
    h.cache.scan({}, {});
    h.run();
    const auto stats = h.cache.stats();
    expect(stats.entries == 2 && stats.bytes == 2 * kEntry && stats.evictions == 1,
           "scan indexed the files, skipped the temp file, and trimmed to the bound");
    expect(!h.files.has_file("/cache/a") && h.files.has_file("/cache/b") &&
               h.files.has_file("/cache/c"),
           "with ties broken by name, the first name was evicted");
}

} // namespace

int run_cache_tests() {
    the_byte_bound_evicts_the_least_recently_used_entry();
    the_entry_bound_evicts_too();
    a_big_put_evicts_several_and_never_itself();
    a_replacement_is_accounted_once_and_an_oversized_put_is_refused();
    a_miss_and_a_corrupt_entry_are_reported_and_dropped();
    remove_is_idempotent_and_releases_the_bytes();
    a_failed_eviction_remove_is_counted_not_fatal();
    scan_rebuilds_the_index_after_a_restart_and_enforces_the_bound();
    return g_failures;
}

} // namespace storage_test
