// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/glyph_raster.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace tessera::ui::text {

struct GlyphAtlasConfig {
    std::uint32_t page_width = 512;
    std::uint32_t page_height = 512;
    std::uint32_t max_pages = 4;
    std::uint32_t padding = 1;
};

// Placement of one glyph in a page. `has_bitmap` is false for whitespace and for glyphs the
// atlas could not place this frame; such entries still carry the advance.
struct GlyphAtlasEntry {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t bearing_x = 0;
    std::int32_t bearing_y = 0;
    float advance = 0.0f;
    std::uint16_t page = 0;
    bool has_bitmap = false;
};

struct GlyphAtlasStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t dropped = 0;
};

// Bounded, deterministic glyph atlas. Pages are packed in insertion order with shelves; when all
// pages are full the least-recently-used page is cleared, but never a page touched in the current
// frame. Pages store RGBA8 (white RGB, coverage alpha) so the batched 2D shader needs no special
// swizzle.
class GlyphAtlas {
public:
    explicit GlyphAtlas(GlyphRasterizer& rasterizer, GlyphAtlasConfig config = {});

    // References stay valid until the next get() that inserts or evicts.
    const GlyphAtlasEntry& get(char32_t codepoint, std::uint16_t pixel_size);
    [[nodiscard]] const GlyphAtlasEntry* find(char32_t codepoint, std::uint16_t pixel_size) const;

    void begin_frame();
    void touch(const GlyphAtlasEntry& entry);

    [[nodiscard]] std::span<const std::uint8_t> page_pixels(std::uint16_t page) const;
    [[nodiscard]] std::uint32_t page_generation(std::uint16_t page) const;
    [[nodiscard]] std::uint32_t page_count() const {
        return static_cast<std::uint32_t>(pages_.size());
    }
    [[nodiscard]] std::uint32_t page_width() const {
        return config_.page_width;
    }
    [[nodiscard]] std::uint32_t page_height() const {
        return config_.page_height;
    }
    [[nodiscard]] const GlyphAtlasStats& stats() const {
        return stats_;
    }

private:
    struct Key {
        char32_t codepoint = 0;
        std::uint16_t pixel_size = 0;

        bool operator==(const Key& other) const {
            return codepoint == other.codepoint && pixel_size == other.pixel_size;
        }
    };

    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            const std::uint64_t value =
                (static_cast<std::uint64_t>(key.codepoint) << 16) | key.pixel_size;
            return std::hash<std::uint64_t>{}(value);
        }
    };

    struct Page {
        std::vector<std::uint8_t> pixels;
        std::uint32_t shelf_y = 0;
        std::uint32_t shelf_height = 0;
        std::uint32_t shelf_x = 0;
        std::uint64_t last_used_frame = 0;
        std::uint32_t generation = 0;
    };

    bool pack(Page& page, std::uint32_t width, std::uint32_t height, std::uint32_t& x,
              std::uint32_t& y);
    void reset_page(Page& page);
    [[nodiscard]] std::int32_t evict_page();
    void upload(Page& page, const GlyphBitmap& bitmap, std::uint32_t x, std::uint32_t y);

    GlyphRasterizer* rasterizer_ = nullptr;
    GlyphAtlasConfig config_;
    std::vector<Page> pages_;
    std::unordered_map<Key, GlyphAtlasEntry, KeyHash> entries_;
    GlyphAtlasEntry uncached_;
    GlyphAtlasStats stats_;
    std::uint64_t frame_ = 0;
};

} // namespace tessera::ui::text
