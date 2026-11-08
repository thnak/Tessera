// SPDX-License-Identifier: MIT

#include "tessera/ui/text/glyph_atlas.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace tessera::ui::text {
namespace {

std::vector<std::uint8_t> make_page_pixels(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 255);
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        pixels[i] = 0;
    }
    return pixels;
}

} // namespace

GlyphAtlas::GlyphAtlas(GlyphRasterizer& rasterizer, GlyphAtlasConfig config)
    : rasterizer_(&rasterizer),
      config_(config) {
    config_.page_width = std::max(config_.page_width, 1u);
    config_.page_height = std::max(config_.page_height, 1u);
    config_.max_pages = std::max(config_.max_pages, 1u);
    config_.padding = std::min(config_.padding, std::min(config_.page_width, config_.page_height));
}

const GlyphAtlasEntry& GlyphAtlas::get(char32_t codepoint, std::uint16_t pixel_size) {
    return get_glyph(rasterizer_->glyph_index(codepoint), pixel_size);
}

const GlyphAtlasEntry& GlyphAtlas::get_glyph(std::uint32_t glyph_index, std::uint16_t pixel_size) {
    const Key key{glyph_index, pixel_size};
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
        ++stats_.hits;
        touch(found->second);
        return found->second;
    }

    ++stats_.misses;
    const GlyphBitmap bitmap = rasterizer_->rasterize_glyph(glyph_index, pixel_size);

    GlyphAtlasEntry entry;
    entry.bearing_x = bitmap.bearing_x;
    entry.bearing_y = bitmap.bearing_y;
    entry.advance = bitmap.advance;

    if (bitmap.width != 0 && bitmap.height != 0) {
        bool placed = false;
        for (std::uint16_t page = 0; page < pages_.size(); ++page) {
            Page& candidate = pages_[page];
            std::uint32_t x = 0;
            std::uint32_t y = 0;
            if (pack(candidate, bitmap.width, bitmap.height, x, y)) {
                entry.page = page;
                entry.x = x;
                entry.y = y;
                entry.width = bitmap.width;
                entry.height = bitmap.height;
                entry.has_bitmap = true;
                candidate.last_used_frame = frame_;
                ++candidate.generation;
                upload(candidate, bitmap, x, y);
                placed = true;
                break;
            }
        }

        if (!placed && pages_.size() < config_.max_pages) {
            Page page;
            page.pixels = make_page_pixels(config_.page_width, config_.page_height);
            pages_.push_back(std::move(page));
            Page& candidate = pages_.back();
            std::uint32_t x = 0;
            std::uint32_t y = 0;
            if (pack(candidate, bitmap.width, bitmap.height, x, y)) {
                entry.page = static_cast<std::uint16_t>(pages_.size() - 1);
                entry.x = x;
                entry.y = y;
                entry.width = bitmap.width;
                entry.height = bitmap.height;
                entry.has_bitmap = true;
                candidate.last_used_frame = frame_;
                ++candidate.generation;
                upload(candidate, bitmap, x, y);
                placed = true;
            }
        }

        if (!placed) {
            const std::int32_t victim = evict_page();
            if (victim >= 0) {
                Page& candidate = pages_[static_cast<std::size_t>(victim)];
                std::uint32_t x = 0;
                std::uint32_t y = 0;
                if (pack(candidate, bitmap.width, bitmap.height, x, y)) {
                    entry.page = static_cast<std::uint16_t>(victim);
                    entry.x = x;
                    entry.y = y;
                    entry.width = bitmap.width;
                    entry.height = bitmap.height;
                    entry.has_bitmap = true;
                    candidate.last_used_frame = frame_;
                    ++candidate.generation;
                    upload(candidate, bitmap, x, y);
                    placed = true;
                }
            }
        }

        if (!placed) {
            ++stats_.dropped;
            uncached_ = entry;
            return uncached_;
        }
    }

    const auto inserted = entries_.emplace(key, entry);
    return inserted.first->second;
}

const GlyphAtlasEntry* GlyphAtlas::find(char32_t codepoint, std::uint16_t pixel_size) const {
    return find_glyph(rasterizer_->glyph_index(codepoint), pixel_size);
}

const GlyphAtlasEntry* GlyphAtlas::find_glyph(std::uint32_t glyph_index,
                                              std::uint16_t pixel_size) const {
    const auto found = entries_.find(Key{glyph_index, pixel_size});
    return found == entries_.end() ? nullptr : &found->second;
}

void GlyphAtlas::begin_frame() {
    ++frame_;
}

void GlyphAtlas::touch(const GlyphAtlasEntry& entry) {
    if (entry.has_bitmap && entry.page < pages_.size()) {
        pages_[entry.page].last_used_frame = frame_;
    }
}

std::span<const std::uint8_t> GlyphAtlas::page_pixels(std::uint16_t page) const {
    if (page >= pages_.size()) {
        return {};
    }
    return pages_[page].pixels;
}

std::uint32_t GlyphAtlas::page_generation(std::uint16_t page) const {
    return page < pages_.size() ? pages_[page].generation : 0;
}

bool GlyphAtlas::pack(Page& page, std::uint32_t width, std::uint32_t height, std::uint32_t& x,
                      std::uint32_t& y) {
    if (width > config_.page_width || height > config_.page_height) {
        return false;
    }
    if (page.shelf_height == 0) {
        page.shelf_y = 0;
        page.shelf_height = height;
        page.shelf_x = 0;
    }

    if (height > page.shelf_height || page.shelf_x + width > config_.page_width) {
        const std::uint32_t next_y =
            page.shelf_y + page.shelf_height + (page.shelf_height == 0 ? 0 : config_.padding);
        if (next_y + height > config_.page_height) {
            return false;
        }
        page.shelf_y = next_y;
        page.shelf_height = height;
        page.shelf_x = 0;
    }

    x = page.shelf_x;
    y = page.shelf_y;
    page.shelf_x += width + config_.padding;
    return true;
}

void GlyphAtlas::reset_page(Page& page) {
    std::fill(page.pixels.begin(), page.pixels.end(), std::uint8_t{255});
    for (std::size_t i = 3; i < page.pixels.size(); i += 4) {
        page.pixels[i] = 0;
    }
    page.shelf_y = 0;
    page.shelf_height = 0;
    page.shelf_x = 0;
    ++page.generation;
}

std::int32_t GlyphAtlas::evict_page() {
    std::uint16_t victim = 0;
    bool found = false;
    for (std::uint16_t page = 0; page < pages_.size(); ++page) {
        const Page& candidate = pages_[page];
        if (candidate.last_used_frame >= frame_) {
            continue;
        }
        if (!found || candidate.last_used_frame < pages_[victim].last_used_frame) {
            victim = page;
            found = true;
        }
    }
    if (!found) {
        return -1;
    }

    Page& page = pages_[victim];
    reset_page(page);
    page.last_used_frame = frame_;
    ++stats_.evictions;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.page == victim) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    return victim;
}

void GlyphAtlas::upload(Page& page, const GlyphBitmap& bitmap, std::uint32_t x, std::uint32_t y) {
    for (std::uint32_t row = 0; row < bitmap.height; ++row) {
        std::uint8_t* destination =
            page.pixels.data() + (static_cast<std::size_t>(y + row) * config_.page_width + x) * 4 +
            3;
        const std::uint8_t* source =
            bitmap.coverage.data() + static_cast<std::size_t>(row) * bitmap.width;
        for (std::uint32_t column = 0; column < bitmap.width; ++column) {
            destination[static_cast<std::size_t>(column) * 4] = source[column];
        }
    }
}

} // namespace tessera::ui::text
