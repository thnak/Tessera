// SPDX-License-Identifier: MIT

#include "tessera/ui/text/text_measure_cache.hpp"

#include <bit>
#include <cstdint>
#include <list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace tessera::ui::text {
namespace {

struct Key {
    std::string text;
    TextStyle style;
    TextConstraints constraints;

    bool operator==(const Key& other) const {
        return text == other.text && style.font_size == other.style.font_size &&
               style.line_height == other.style.line_height &&
               style.letter_spacing == other.style.letter_spacing &&
               style.font_weight == other.style.font_weight && style.italic == other.style.italic &&
               style.white_space == other.style.white_space &&
               constraints.max_width == other.constraints.max_width &&
               constraints.wrap == other.constraints.wrap;
    }
};

void mix(std::size_t& seed, std::size_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

std::size_t float_bits(float value) {
    return static_cast<std::size_t>(std::bit_cast<std::uint32_t>(value));
}

struct KeyHash {
    std::size_t operator()(const Key& key) const {
        std::size_t seed = std::hash<std::string>{}(key.text);
        mix(seed, float_bits(key.style.font_size));
        mix(seed, float_bits(key.style.line_height));
        mix(seed, float_bits(key.style.letter_spacing));
        mix(seed, static_cast<std::size_t>(key.style.font_weight));
        mix(seed, static_cast<std::size_t>(key.style.italic));
        mix(seed, static_cast<std::size_t>(key.style.white_space));
        mix(seed, float_bits(key.constraints.max_width));
        mix(seed, static_cast<std::size_t>(key.constraints.wrap));
        return seed;
    }
};

struct Entry {
    Key key;
    TextMetrics metrics;
};

} // namespace

struct TextMeasureCache::Impl {
    Impl(std::shared_ptr<const TextMeasurer> text_measurer, std::size_t max_entries)
        : measurer(std::move(text_measurer)),
          capacity(max_entries) {}

    std::shared_ptr<const TextMeasurer> measurer;
    std::size_t capacity = 0;
    std::list<Entry> entries;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index;
    Stats stats;
};

TextMeasureCache::TextMeasureCache(std::shared_ptr<const TextMeasurer> measurer,
                                   std::size_t capacity)
    : impl_(std::make_unique<Impl>(std::move(measurer), capacity)) {
    if (impl_->measurer == nullptr) {
        throw std::invalid_argument("text measure cache requires a measurer");
    }
    if (impl_->capacity == 0) {
        throw std::invalid_argument("text measure cache capacity must be greater than zero");
    }
}

TextMeasureCache::~TextMeasureCache() = default;
TextMeasureCache::TextMeasureCache(TextMeasureCache&&) noexcept = default;
TextMeasureCache& TextMeasureCache::operator=(TextMeasureCache&&) noexcept = default;

TextMetrics TextMeasureCache::measure(std::string_view text, const TextStyle& style,
                                      const TextConstraints& constraints) {
    Key key{std::string(text), style, constraints};
    const auto found = impl_->index.find(key);
    if (found != impl_->index.end()) {
        impl_->entries.splice(impl_->entries.begin(), impl_->entries, found->second);
        ++impl_->stats.hits;
        return found->second->metrics;
    }

    ++impl_->stats.misses;
    const TextMetrics metrics = impl_->measurer->measure(text, style, constraints);

    impl_->entries.push_front(Entry{std::move(key), metrics});
    impl_->index.emplace(impl_->entries.front().key, impl_->entries.begin());

    if (impl_->entries.size() > impl_->capacity) {
        auto last = std::prev(impl_->entries.end());
        impl_->index.erase(last->key);
        impl_->entries.erase(last);
        ++impl_->stats.evictions;
    }
    return metrics;
}

std::size_t TextMeasureCache::invalidate(std::string_view text) {
    std::size_t removed = 0;
    for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
        if (it->key.text == text) {
            impl_->index.erase(it->key);
            it = impl_->entries.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    impl_->stats.invalidations += removed;
    return removed;
}

void TextMeasureCache::clear() {
    impl_->entries.clear();
    impl_->index.clear();
    impl_->stats = Stats{};
}

std::size_t TextMeasureCache::size() const {
    return impl_->entries.size();
}

std::size_t TextMeasureCache::capacity() const {
    return impl_->capacity;
}

TextMeasureCache::Stats TextMeasureCache::stats() const {
    Stats result = impl_->stats;
    result.entries = impl_->entries.size();
    return result;
}

} // namespace tessera::ui::text
