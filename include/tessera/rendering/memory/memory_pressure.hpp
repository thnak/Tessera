// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace tessera::rendering::memory {

// One evictable cache, as the pressure path sees it (issue #705).
//
// The pressure path does not know what a cache *is*. It knows three things about each one: how many
// bytes it currently holds, how to drop everything that the current frame does not reference, and a
// name for the diagnostic. That is deliberately the whole interface: a cache that cannot answer
// `evict_idle()` honestly (because it cannot tell what the current frame uses) must not register,
// rather than registering and dropping pixels the frame is about to draw.
struct EvictableCache {
    // Stable name for the diagnostic and the tests ("image_texture", "path_texture", ...).
    std::string_view name;
    // Live bytes this cache holds right now. Called once per pressure event, not per frame.
    std::function<std::uint64_t()> live_bytes;
    // Drops every entry the current frame does not reference, and returns the bytes released.
    // MUST NOT drop anything the current frame's recorded commands reference: memory-model-v1
    // section 5 says eviction must not change pixels, and a cache that cannot honour that is a bug
    // in the cache, not in this path.
    std::function<std::uint64_t()> evict_idle;
};

// What one pressure event did, for the diagnostic and for the tests.
struct PressureResult {
    // Bytes released, summed over the caches that were asked.
    std::uint64_t released_bytes = 0;
    // How many caches were asked. A cache with nothing to release still counts: "asked and had
    // nothing" is different from "not asked".
    std::uint32_t caches_evicted = 0;
    // True when the caller should retry the allocation that failed. False when nothing was
    // released, because a retry that cannot succeed only burns the one attempt the spec allows.
    bool should_retry = false;
};

// The ordered memory-pressure entry point (issue #705, memory-model-v1 section 7).
//
// Evicts in a fixed, documented order and reports what it freed. The order is **largest-first
// strictly** - image texture, path texture, paint, glyph atlas, measure - which is the owner's
// decision recorded on the issue: it maximises bytes freed per step, and the two big texture caches
// (64 MiB and 32 MiB by default) are also the two that are cheapest to rebuild from their sources.
//
// The order is a property of this function, not of the registration order, so a caller cannot
// accidentally change it by registering caches in a different sequence. A cache whose name is not
// in the order is evicted last, in registration order, rather than being silently skipped: a new
// cache that nobody ordered is still better off evicted than resident.
class MemoryPressure {
public:
    // The documented order. Exposed so a test can assert it rather than restate it.
    [[nodiscard]] static std::span<const std::string_view> eviction_order() noexcept;

    // Registers a cache. Registration order does not affect eviction order.
    void add(EvictableCache cache);

    // Runs one pressure event: evicts every registered cache in the documented order, accumulating
    // the bytes released. `should_retry` is true only when something was actually released.
    //
    // This is the "evict caches, retry once" half of section 7. The caller owns the retry: it calls
    // this, and if `should_retry` it attempts the allocation exactly once more. The retry is not
    // here because only the caller knows what allocation failed.
    [[nodiscard]] PressureResult relieve();

    [[nodiscard]] std::uint64_t total_live_bytes() const;
    [[nodiscard]] std::size_t cache_count() const noexcept {
        return caches_.size();
    }

private:
    std::vector<EvictableCache> caches_;
};

// The rank of a cache name in the documented order, or `eviction_order().size()` for a name that is
// not in it. Exposed for the test that asserts the order, and used by `relieve()`.
[[nodiscard]] std::size_t eviction_rank(std::string_view name) noexcept;

} // namespace tessera::rendering::memory
