// SPDX-License-Identifier: MIT

#include "tessera/rendering/image/image_texture_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <list>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tessera::rendering::image {
namespace {

constexpr std::uint32_t kBytesPerPixel = 4;

struct ImageTextureKeyHash {
    [[nodiscard]] std::size_t operator()(const ImageTextureKey& key) const noexcept;
};

[[nodiscard]] std::size_t hash_key(const ImageTextureKey& key) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    mix(key.content_hash);
    mix(key.width);
    mix(key.height);
    mix(static_cast<std::uint64_t>(key.format));
    return static_cast<std::size_t>(hash);
}

std::size_t ImageTextureKeyHash::operator()(const ImageTextureKey& key) const noexcept {
    return hash_key(key);
}

// Deterministic nearest resampling to the requested raster extent: the destination pixel center
// maps to a source pixel, exactly like CpuCanvas::blit_texture, so the CPU golden and the uploaded
// texture agree. No filtering, no floating point: the mapping is integer math.
[[nodiscard]] std::vector<std::uint8_t>
resample_nearest(const DecodedImage& image, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * kBytesPerPixel, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint64_t numerator_y = (2ULL * y + 1ULL) * image.height;
        const std::uint32_t source_y = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(numerator_y / (2ULL * height)), image.height - 1U);
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint64_t numerator_x = (2ULL * x + 1ULL) * image.width;
            const std::uint32_t source_x = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(numerator_x / (2ULL * width)), image.width - 1U);
            const std::size_t source =
                (static_cast<std::size_t>(source_y) * image.width + source_x) * kBytesPerPixel;
            const std::size_t destination =
                (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel;
            for (std::uint32_t channel = 0; channel < kBytesPerPixel; ++channel) {
                pixels[destination + channel] = image.rgba[source + channel];
            }
        }
    }
    return pixels;
}

} // namespace

struct ImageTextureCache::Impl {
    struct Entry {
        ImageTextureKey key;
        std::unique_ptr<rhi::Texture> texture;
        rhi::Extent2D extent;
        std::uint64_t bytes = 0;
    };

    Impl(rhi::Device& target_device, ImageTextureCacheConfig cache_config)
        : device(target_device),
          config(std::move(cache_config)) {
        config.max_entries = std::max(1U, config.max_entries);
        config.max_bytes = std::max<std::uint64_t>(kBytesPerPixel, config.max_bytes);
    }

    // D7 health poll: a Lost -> Ready transition is a new texture generation, so every texture
    // created before the loss is invalid and the cache is empty when rendering resumes.
    [[nodiscard]] bool observe_device() {
        const rhi::DeviceStatus status = device.status();
        if (status == rhi::DeviceStatus::Lost) {
            if (!device_lost) {
                device_lost = true;
                clear_entries();
                ++stats.invalidations;
            }
        } else if (device_lost) {
            device_lost = false;
            ++generation;
            clear_entries();
            ++stats.invalidations;
        }
        return device_lost;
    }

    void clear_entries() {
        entries.clear(); // unique_ptr destruction is the only release path: RHI owns the memory
        index.clear();
        total_bytes = 0;
    }

    void evict_until_fits(std::uint64_t incoming_bytes) {
        while (!entries.empty() && (entries.size() >= config.max_entries ||
                                    total_bytes + incoming_bytes > config.max_bytes)) {
            const Entry& victim = entries.back();
            total_bytes -= victim.bytes;
            index.erase(victim.key);
            entries.pop_back();
            ++stats.evictions;
        }
    }

    [[nodiscard]] Stats snapshot() const {
        Stats result = stats;
        result.entries = static_cast<std::uint32_t>(entries.size());
        result.bytes = total_bytes;
        return result;
    }

    rhi::Device& device;
    ImageTextureCacheConfig config;
    std::list<Entry> entries;
    std::unordered_map<ImageTextureKey, std::list<Entry>::iterator, ImageTextureKeyHash> index;
    std::uint64_t total_bytes = 0;
    std::uint64_t generation = 0;
    bool device_lost = false;
    Stats stats;
};

ImageTextureCache::ImageTextureCache(rhi::Device& device, ImageTextureCacheConfig config)
    : impl_(std::make_unique<Impl>(device, config)) {}

ImageTextureCache::~ImageTextureCache() = default;

std::expected<CachedImageTexture, ImageTextureError>
ImageTextureCache::acquire(const DecodedImage& image, const ImageTextureKey& key) {
    if (impl_->observe_device()) {
        return std::unexpected(ImageTextureError::device_lost);
    }
    if (key.width == 0U || key.height == 0U || key.content_hash == 0U || image.width == 0U ||
        image.height == 0U ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4U) {
        return std::unexpected(ImageTextureError::invalid_request);
    }
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(key.width) * kBytesPerPixel;
    const std::uint64_t entry_bytes = row_bytes * key.height;
    if (entry_bytes > impl_->config.max_bytes) {
        return std::unexpected(ImageTextureError::entry_too_large);
    }

    if (const auto found = impl_->index.find(key); found != impl_->index.end()) {
        impl_->entries.splice(impl_->entries.begin(), impl_->entries, found->second);
        ++impl_->stats.hits;
        return CachedImageTexture{found->second->texture.get(), found->second->extent};
    }

    ++impl_->stats.misses;
    std::vector<std::uint8_t> pixels = resample_nearest(image, key.width, key.height);
    rhi::TextureDesc desc;
    desc.extent = rhi::Extent2D{key.width, key.height};
    desc.format = key.format;
    desc.pixels = pixels.data();
    desc.row_pitch = static_cast<std::size_t>(row_bytes);
    desc.filter = impl_->config.filter;
    desc.address = impl_->config.address;

    std::unique_ptr<rhi::Texture> texture;
    try {
        texture = impl_->device.create_texture(desc);
    } catch (const std::exception&) {
        return std::unexpected(ImageTextureError::upload_failed);
    }
    if (!texture) {
        return std::unexpected(ImageTextureError::upload_failed);
    }
    ++impl_->stats.uploads;

    impl_->evict_until_fits(entry_bytes);
    impl_->entries.push_front(Impl::Entry{key, std::move(texture), desc.extent, entry_bytes});
    impl_->index[key] = impl_->entries.begin();
    impl_->total_bytes += entry_bytes;
    return CachedImageTexture{impl_->entries.front().texture.get(), impl_->entries.front().extent};
}

void ImageTextureCache::invalidate_all() {
    impl_->clear_entries();
    ++impl_->stats.invalidations;
}

void ImageTextureCache::notify_device_lost() {
    impl_->clear_entries();
    impl_->device_lost = true;
    ++impl_->stats.invalidations;
}

std::uint64_t ImageTextureCache::generation() const {
    return impl_->generation;
}

std::uint32_t ImageTextureCache::entry_count() const {
    return static_cast<std::uint32_t>(impl_->entries.size());
}

std::uint64_t ImageTextureCache::byte_count() const {
    return impl_->total_bytes;
}

ImageTextureCache::Stats ImageTextureCache::stats() const {
    return impl_->snapshot();
}

} // namespace tessera::rendering::image
