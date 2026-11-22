// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/background_layers.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace tessera::ui::paint {

struct BackgroundLayerCacheConfig {
    // Both budgets are enforced; the newest entry is always kept when it fits. Entries are evicted
    // least-recently-used first, in the same order on every run. One entry is dominated by the
    // cached `BackgroundLayerList` (1672 bytes, issue #731), so 64 entries of distinct values fit
    // the 256 KiB budget; a theme sharing one value across many nodes needs a single entry.
    std::size_t max_entries = 64;
    std::size_t max_bytes = 256 * 1024;
};

struct BackgroundLayerCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t invalidations = 0;
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

// Caller-owned bounded LRU of parsed `background-image` layer lists (issue #733 item 2).
//
// Why it exists: `paint_gradient` is reached once per painted node per frame, and for a stylesheet
// value it called `style::parse_background_layers(*style.find("background-image"))`. That is a full
// re-parse of the authored text -- a `split_commas` vector and a `lower(part)` string per layer --
// on every frame for every node with a `background-image`, and the result was thrown away when the
// call returned. The paint cache does not save it: a hit replays recorded ops and never reaches
// `paint_gradient`, so only a *miss* pays the parse, but the first frame, every dirty node and
// every PaintView pay it in full.
//
// The key is the style's own identity (`ComputedStyle::identity`, the shared-entries block PR #730
// introduced), not the value text. Two consequences, both deliberate:
//   - A node whose restyle kept its entries -- which the style tree's memo guarantees for a node
//     outside every restyled subtree -- hits without comparing a single byte of the value.
//   - The parse is *style*-scoped, not node-scoped, so a theme that gives 5,000 nodes the same
//     `background-image` parses it once. That is the shape the cost has: one authored value read by
//     many nodes.
//
// `identity` is a `shared_ptr` to the entries block, so an entry also keeps the text it was parsed
// from alive: the address cannot be reused by a different style while a parse result refers to it.
// An identity of null (a default-constructed style with no entries) is never cached, because every
// such style shares it and none of them owns a value.
//
// A `nullopt` from the parser is cached too -- an invalid value is the most expensive case to
// re-parse and the most stable, and a caller paints nothing either way.
class BackgroundLayerCache {
public:
    // The resolved answer for one style: either the layer list, or "this value does not parse".
    // `parsed` mirrors `style::parse_background_layers`' own `std::optional` contract, so a caller
    // cannot tell a cached invalid value from a fresh one.
    struct Value {
        std::optional<style::BackgroundLayerList> parsed;
    };

    explicit BackgroundLayerCache(BackgroundLayerCacheConfig config = {});

    BackgroundLayerCache(const BackgroundLayerCache&) = delete;
    BackgroundLayerCache& operator=(const BackgroundLayerCache&) = delete;

    // The parsed value of `style`'s `background-image`, parsing and caching it on a miss. `text` is
    // the authored value the caller already looked up, so the cache never re-reads the style. A
    // style with no cacheable identity is parsed without being stored.
    const Value& get(const style::ComputedStyle& style, std::string_view text);

    void invalidate();
    void clear();
    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const {
        return config_.max_entries > 0 && config_.max_bytes > 0 && enabled_;
    }

    [[nodiscard]] BackgroundLayerCacheStats stats() const;

private:
    // The map is keyed by the owned identity block's address, so the key stays stable and the
    // entry keeps the block alive. 64-bit because two different styles cannot share an address
    // while both are alive, which the held `shared_ptr` guarantees.
    using Key = std::uintptr_t;

    struct Entry {
        std::shared_ptr<const void> owner;
        Value value;
        std::size_t bytes = 0;
        std::list<Key>::iterator lru;
    };

    static std::size_t entry_bytes();
    void evict_one();

    BackgroundLayerCacheConfig config_;
    std::unordered_map<Key, Entry> entries_;
    // Most recently used at the front. Holds the same keys as `entries_`, so the LRU order is
    // deterministic and independent of the unordered_map's bucket layout.
    std::list<Key> lru_;
    std::size_t bytes_ = 0;
    bool enabled_ = true;
    Value scratch_;
    BackgroundLayerCacheStats counters_;
};

} // namespace tessera::ui::paint
