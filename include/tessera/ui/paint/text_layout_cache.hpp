// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <string>
#include <string_view>

namespace tessera::ui::paint {

// Metric-source identifier: 0 is the deterministic stub metrics provider the paint pass uses today.
// Callers that measure with a real font face must use a stable, distinct id (and invalidate the
// cache when the face/generation changes).
inline constexpr std::uint32_t kStubTextMetricsId = 0;

struct TextLayoutCacheConfig {
    // Both budgets are enforced; the newest entry is always kept when it fits. Entries are evicted
    // least-recently-used first. Exact byte accounting (text_layout_cache.cpp):
    // sizeof(Entry) + 2 * (sizeof(Key) + text.capacity()) [map key + LRU key] + 2 pointers
    // (LRU node) + lines.capacity() * sizeof(text::TextLine).
    std::size_t max_entries = 64;
    std::size_t max_bytes = 256 * 1024;
};

struct TextLayoutCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t invalidations = 0;
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

// Caller-owned bounded LRU cache for wrapped paint text line boxes. Deterministic key: text bytes,
// content-box wrap width, font size/weight/italic/letter-spacing/line-height, wrap mode, max_lines,
// and the metrics-source id. Float key fields are compared with exact IEEE equality (no epsilon).
//
// A returned reference is stable until the next call that inserts or evicts (or invalidate/clear):
// a hit performs no allocation and only refreshes the LRU position. There is no global singleton;
// the paint frame/sink owns one instance and passes it through PaintContext. `set_enabled(false)`
// bypasses the cache (miss-only scratch path) for tests/benchmarks.
class TextLayoutCache {
public:
    explicit TextLayoutCache(TextLayoutCacheConfig config = {});

    TextLayoutCache(const TextLayoutCache&) = delete;
    TextLayoutCache& operator=(const TextLayoutCache&) = delete;

    // Returns the line boxes for the request, computing and caching them on a miss. `max_lines ==
    // 0` means unlimited and is part of the key; the cached value is already truncated.
    const text::TextLayout& get(std::string_view text, const text::TextStyle& style,
                                const text::FontMetrics& metrics,
                                const text::TextConstraints& constraints,
                                std::uint32_t metrics_source_id, std::uint32_t max_lines);

    // Drops every entry (font/metrics generation change).
    void invalidate();
    void clear();
    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const {
        return config_.max_entries > 0 && config_.max_bytes > 0 && enabled_;
    }

    [[nodiscard]] TextLayoutCacheStats stats() const;

private:
    struct KeyView {
        std::string_view text;
        float width = 0.0f;
        float font_size = 0.0f;
        float letter_spacing = 0.0f;
        float line_height = 0.0f;
        int font_weight = 0;
        bool italic = false;
        bool wrap = false;
        std::uint32_t max_lines = 0;
        std::uint32_t metrics_source_id = 0;
    };

    struct Key {
        std::string text;
        float width = 0.0f;
        float font_size = 0.0f;
        float letter_spacing = 0.0f;
        float line_height = 0.0f;
        int font_weight = 0;
        bool italic = false;
        bool wrap = false;
        std::uint32_t max_lines = 0;
        std::uint32_t metrics_source_id = 0;
    };

    struct KeyLess {
        using is_transparent = void;

        bool operator()(const Key& lhs, const Key& rhs) const {
            return less(lhs, rhs);
        }
        bool operator()(const Key& lhs, const KeyView& rhs) const {
            return less(lhs, rhs);
        }
        bool operator()(const KeyView& lhs, const Key& rhs) const {
            return less(lhs, rhs);
        }

    private:
        // Total order over the key fields; floats compare by their exact IEEE bit pattern (no
        // epsilon), so -0.0 and +0.0 are distinct keys and NaN is not possible from paint input.
        template<typename Left, typename Right>
        static bool less(const Left& lhs, const Right& rhs) {
            const auto bits = [](float value) {
                std::uint32_t result = 0;
                static_assert(sizeof(result) == sizeof(value));
                std::memcpy(&result, &value, sizeof(result));
                return result;
            };
            if (lhs.metrics_source_id != rhs.metrics_source_id) {
                return lhs.metrics_source_id < rhs.metrics_source_id;
            }
            if (lhs.max_lines != rhs.max_lines) {
                return lhs.max_lines < rhs.max_lines;
            }
            if (lhs.wrap != rhs.wrap) {
                return lhs.wrap < rhs.wrap;
            }
            if (lhs.font_weight != rhs.font_weight) {
                return lhs.font_weight < rhs.font_weight;
            }
            if (lhs.italic != rhs.italic) {
                return lhs.italic < rhs.italic;
            }
            if (bits(lhs.font_size) != bits(rhs.font_size)) {
                return bits(lhs.font_size) < bits(rhs.font_size);
            }
            if (bits(lhs.letter_spacing) != bits(rhs.letter_spacing)) {
                return bits(lhs.letter_spacing) < bits(rhs.letter_spacing);
            }
            if (bits(lhs.line_height) != bits(rhs.line_height)) {
                return bits(lhs.line_height) < bits(rhs.line_height);
            }
            if (bits(lhs.width) != bits(rhs.width)) {
                return bits(lhs.width) < bits(rhs.width);
            }
            return lhs.text < rhs.text;
        }
    };

    // LRU list stores the key so eviction can erase the map entry; map nodes stay stable. The
    // returned layout reference is therefore stable until an insert/evict, not until a hit.
    using Lru = std::list<Key>;

    struct Entry {
        text::TextLayout layout;
        std::size_t bytes = 0;
        Lru::iterator lru;
    };

    using Map = std::map<Key, Entry, KeyLess>;

    static std::size_t entry_bytes(const Key& key, const text::TextLayout& layout);
    void evict_one();
    void insert(Key key, text::TextLayout layout);
    [[nodiscard]] static text::TextLayout build(std::string_view text, const text::TextStyle& style,
                                                const text::FontMetrics& metrics,
                                                const text::TextConstraints& constraints,
                                                std::uint32_t max_lines);

    TextLayoutCacheConfig config_;
    bool enabled_ = true;
    Map entries_;
    Lru lru_;
    std::size_t bytes_ = 0;
    text::TextLayout scratch_;
    TextLayoutCacheStats counters_;
};

} // namespace tessera::ui::paint
