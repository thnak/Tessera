// SPDX-License-Identifier: MIT
//
// The null backend's buffer and texture, shared by `null_device.cpp` and the commands that move
// bytes between them (RHI 1.17 `copy_buffer_to_texture`). Split out of `null_device.cpp`, which is
// at its size limit. Private to the null backend: nothing outside `src/rendering/null` includes it.

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace tessera::rendering::null {

class NullBuffer final : public rhi::Buffer {
public:
    NullBuffer(std::size_t size, rhi::BufferUsage usage)
        : data_(size, 0),
          usage_(usage) {}

    [[nodiscard]] std::size_t size() const override {
        return data_.size();
    }
    [[nodiscard]] rhi::BufferUsage usage() const {
        return usage_;
    }

    void write(const void* data, std::size_t size, std::size_t offset) override {
        check_range(size, offset);
        std::memcpy(data_.data() + offset, data, size);
    }

    void read(void* data, std::size_t size, std::size_t offset) const override {
        check_range(size, offset);
        std::memcpy(data, data_.data() + offset, size);
    }

private:
    // Matches the Vulkan backend: a range outside the buffer is a caller bug, not a no-op.
    void check_range(std::size_t size, std::size_t offset) const {
        if (size > data_.size() || offset > data_.size() - size) {
            throw std::out_of_range("NullBuffer: range exceeds the buffer size");
        }
    }

    std::vector<std::uint8_t> data_;
    rhi::BufferUsage usage_ = rhi::BufferUsage::Vertex;
};

class NullTexture final : public rhi::Texture {
public:
    explicit NullTexture(const rhi::TextureDesc& desc)
        : desc_(desc) {
        if (!rhi::is_supported_sample_count(desc.sample_count)) {
            throw std::invalid_argument("NullTexture: unsupported sample count");
        }
        if (desc.sample_count > 1 && desc.pixels != nullptr) {
            throw std::invalid_argument(
                "NullTexture: pixels cannot be uploaded to a multisampled texture");
        }
        if (!rhi::valid_texture_levels(desc)) {
            throw std::invalid_argument("NullTexture: invalid mip level count");
        }
        const std::size_t bytes = bytes_per_pixel(desc.format);
        data_.assign(static_cast<std::size_t>(desc.extent.width) * desc.extent.height * bytes, 0);
        if (desc.pixels != nullptr && bytes != 0) {
            const std::size_t row_pitch = desc.row_pitch != 0
                                              ? desc.row_pitch
                                              : static_cast<std::size_t>(desc.extent.width) * bytes;
            for (std::uint32_t row = 0; row < desc.extent.height; ++row) {
                std::memcpy(data_.data() +
                                static_cast<std::size_t>(row) * desc.extent.width * bytes,
                            static_cast<const std::uint8_t*>(desc.pixels) + row * row_pitch,
                            static_cast<std::size_t>(desc.extent.width) * bytes);
            }
        }
    }

    [[nodiscard]] rhi::Extent2D extent() const override {
        return desc_.extent;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return desc_.format;
    }
    [[nodiscard]] std::uint32_t sample_count() const override {
        return desc_.sample_count;
    }
    [[nodiscard]] std::uint32_t levels() const override {
        return desc_.levels;
    }
    void write(const void* data, std::size_t size) override {
        if (desc_.levels > 1) {
            throw std::invalid_argument("NullTexture: mipmapped textures are immutable");
        }
        if (desc_.sample_count > 1) {
            throw std::invalid_argument("NullTexture: multisampled textures cannot be uploaded");
        }
        if (data == nullptr || size != data_.size()) {
            return;
        }
        std::memcpy(data_.data(), data, size);
    }
    void write_region(const rhi::TextureRegion& region, const void* data,
                      std::size_t row_pitch) override {
        if (desc_.levels > 1) {
            throw std::invalid_argument("NullTexture: mipmapped textures are immutable");
        }
        if (desc_.sample_count > 1) {
            throw std::invalid_argument("NullTexture: multisampled textures cannot be uploaded");
        }
        const std::size_t bytes = bytes_per_pixel(desc_.format);
        const std::size_t pitch = rhi::checked_region_pitch(desc_.extent, bytes, region, row_pitch);
        if (region.empty()) {
            return;
        }
        if (data == nullptr) {
            throw std::invalid_argument("NullTexture: region upload without data");
        }
        const std::size_t row_bytes = static_cast<std::size_t>(region.width) * bytes;
        for (std::uint32_t row = 0; row < region.height; ++row) {
            std::memcpy(
                data_.data() +
                    (static_cast<std::size_t>(region.y + row) * desc_.extent.width + region.x) *
                        bytes,
                static_cast<const std::uint8_t*>(data) + static_cast<std::size_t>(row) * pitch,
                row_bytes);
        }
    }

private:
    static std::size_t bytes_per_pixel(rhi::TextureFormat format) {
        return format == rhi::TextureFormat::R8Unorm ? 1 : 4;
    }

    rhi::TextureDesc desc_;
    std::vector<std::uint8_t> data_;
};

} // namespace tessera::rendering::null
