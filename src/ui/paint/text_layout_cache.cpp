// SPDX-License-Identifier: MIT

#include "tessera/ui/paint/text_layout_cache.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace tessera::ui::paint {
namespace {

constexpr std::size_t kLruNodePointers = 2 * sizeof(void*);

} // namespace

TextLayoutCache::TextLayoutCache(TextLayoutCacheConfig config)
    : config_(config) {}

std::size_t TextLayoutCache::entry_bytes(const Key& key, const text::TextLayout& layout) {
    // Exact allocation accounting: the map key and the LRU list key each hold one std::string
    // (capacity, not just size), the LRU node holds two pointers, and the line box vector's
    // capacity is charged. Fixed object overheads are sizeof(Entry)/sizeof(Key).
    return sizeof(Entry) + 2 * (sizeof(Key) + key.text.capacity()) + kLruNodePointers +
           layout.lines.capacity() * sizeof(text::TextLine);
}

text::TextLayout TextLayoutCache::build(std::string_view text, const text::TextStyle& style,
                                        const text::TextConstraints& constraints,
                                        const PaintTextMetricsSource& source,
                                        std::uint32_t max_lines) {
    text::TextLayout layout = source.layout(text, style, constraints);
    if (max_lines > 0 && layout.lines.size() > max_lines) {
        layout.lines.resize(max_lines);
    }
    return layout;
}

const text::TextLayout& TextLayoutCache::get(std::string_view text, const text::TextStyle& style,
                                             const text::TextConstraints& constraints,
                                             const PaintTextMetricsSource& source,
                                             std::uint32_t max_lines) {
    const bool wrap = constraints.wrap && text::white_space_wraps(style.white_space) &&
                      std::isfinite(constraints.max_width);
    const KeyView view{text,
                       wrap ? constraints.max_width : 0.0f,
                       style.font_size,
                       style.letter_spacing,
                       style.word_spacing,
                       style.line_height,
                       style.font_weight,
                       style.italic,
                       wrap,
                       style.white_space,
                       style.text_transform,
                       max_lines,
                       source.id()};

    if (!enabled()) {
        ++counters_.misses;
        scratch_ = build(text, style, constraints, source, max_lines);
        return scratch_;
    }

    const auto found = entries_.find(view);
    if (found != entries_.end()) {
        ++counters_.hits;
        // Splice only moves the existing list node; a hit allocates nothing.
        lru_.splice(lru_.begin(), lru_, found->second.lru);
        return found->second.layout;
    }

    ++counters_.misses;
    text::TextLayout layout = build(text, style, constraints, source, max_lines);
    Key key{std::string(text),     view.width,          view.font_size,
            view.letter_spacing,   view.word_spacing,   view.line_height,
            view.font_weight,      view.italic,         view.wrap,
            view.white_space,      view.text_transform, view.max_lines,
            view.metrics_source_id};
    const std::size_t bytes = entry_bytes(key, layout);
    if (bytes > config_.max_bytes) {
        // The entry alone exceeds the byte budget: return it through scratch instead of breaking
        // the configured bound.
        scratch_ = std::move(layout);
        return scratch_;
    }

    auto [inserted, ok] = entries_.try_emplace(std::move(key), Entry{});
    (void)ok;
    inserted->second.layout = std::move(layout);
    inserted->second.bytes = entry_bytes(inserted->first, inserted->second.layout);
    inserted->second.lru = lru_.insert(lru_.begin(), inserted->first);
    bytes_ += inserted->second.bytes;
    while ((entries_.size() > config_.max_entries || bytes_ > config_.max_bytes) &&
           lru_.size() > 1) {
        evict_one();
    }
    return inserted->second.layout;
}

void TextLayoutCache::evict_one() {
    if (lru_.empty()) {
        return;
    }
    const Key& victim = lru_.back();
    const auto found = entries_.find(victim);
    if (found != entries_.end()) {
        bytes_ -= found->second.bytes;
        entries_.erase(found);
        ++counters_.evictions;
    }
    lru_.pop_back();
}

void TextLayoutCache::invalidate() {
    clear();
    ++counters_.invalidations;
}

void TextLayoutCache::clear() {
    entries_.clear();
    lru_.clear();
    bytes_ = 0;
    scratch_ = text::TextLayout{};
}

void TextLayoutCache::set_enabled(bool enabled) {
    enabled_ = enabled;
}

TextLayoutCacheStats TextLayoutCache::stats() const {
    TextLayoutCacheStats result = counters_;
    result.entries = entries_.size();
    result.bytes = bytes_;
    return result;
}

} // namespace tessera::ui::paint
