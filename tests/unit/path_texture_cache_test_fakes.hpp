// SPDX-License-Identifier: MIT
//
// Test doubles for the PathTextureCache unit tests. They stay Vulkan-free on purpose: the cache
// contract (keying, LRU bounds, D7 generation invalidation, failure values) must be verifiable on
// every platform in every build configuration.

#pragma once

#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::test::path_texture {

namespace path = tessera::rendering::path;
namespace rhi = tessera::rendering::rhi;

[[nodiscard]] inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "path_texture_cache: " << message << "\n";
    }
    return condition;
}

[[nodiscard]] inline std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

// Deterministic CPU-only rasterizer: fills the target with a fixed gradient so tests can compare
// uploaded bytes without depending on ThorVG (which is optional) or a GPU (which tests must not
// require).
class PatternRasterizer final : public path::PathRasterizer {
public:
    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "pattern";
    }

    [[nodiscard]] std::expected<path::RasterStats, path::RasterError>
    rasterize(std::span<const path::PathShape> shapes, const path::RasterTarget& target) override {
        ++calls;
        if (fail) {
            return std::unexpected(path::RasterError::backend_failure);
        }
        if (!target.valid()) {
            return std::unexpected(path::RasterError::invalid_target);
        }
        for (std::uint32_t y = 0; y < target.height; ++y) {
            std::uint8_t* row =
                target.pixels.data() + static_cast<std::size_t>(y) * target.stride_bytes;
            for (std::uint32_t x = 0; x < target.width; ++x) {
                row[x * 4U + 0U] = static_cast<std::uint8_t>((x * 7U + y * 3U) & 0xFFU);
                row[x * 4U + 1U] = static_cast<std::uint8_t>((x * 3U + y * 11U) & 0xFFU);
                row[x * 4U + 2U] = static_cast<std::uint8_t>(128U + ((x + y) & 0x3FU));
                row[x * 4U + 3U] = 255;
            }
        }
        return path::RasterStats{static_cast<std::uint32_t>(shapes.size())};
    }

    std::uint32_t calls = 0;
    bool fail = false;
};

// Stores the uploaded bytes so tests can assert the cache uploaded exactly what the rasterizer
// produced. `CachedPathTexture::texture` is downcast with dynamic_cast in the tests.
class FakeTexture final : public rhi::Texture {
public:
    explicit FakeTexture(const rhi::TextureDesc& desc)
        : extent_(desc.extent),
          format_(desc.format) {
        const std::size_t bytes_per_pixel = desc.format == rhi::TextureFormat::R8Unorm ? 1U : 4U;
        bytes_.assign(static_cast<std::size_t>(extent_.width) * extent_.height * bytes_per_pixel,
                      0);
        if (desc.pixels == nullptr || bytes_.empty()) {
            return;
        }
        const std::size_t row_pitch =
            desc.row_pitch != 0 ? desc.row_pitch
                                : static_cast<std::size_t>(extent_.width) * bytes_per_pixel;
        for (std::uint32_t row = 0; row < extent_.height; ++row) {
            std::memcpy(bytes_.data() +
                            static_cast<std::size_t>(row) * extent_.width * bytes_per_pixel,
                        static_cast<const std::uint8_t*>(desc.pixels) + row * row_pitch,
                        static_cast<std::size_t>(extent_.width) * bytes_per_pixel);
        }
    }

    [[nodiscard]] rhi::Extent2D extent() const override {
        return extent_;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return format_;
    }
    void write(const void* data, std::size_t size) override {
        if (data != nullptr && size == bytes_.size()) {
            std::memcpy(bytes_.data(), data, size);
        }
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const {
        return bytes_;
    }

private:
    rhi::Extent2D extent_;
    rhi::TextureFormat format_;
    std::vector<std::uint8_t> bytes_;
};

// Minimal device stub: creates textures that keep their bytes and lets a test flip D7 health and
// inject texture-creation failures. Every other RHI factory returns nullptr because the cache never
// calls it.
class FakeDevice final : public rhi::Device {
public:
    [[nodiscard]] rhi::Backend backend() const override {
        return rhi::Backend::Null;
    }

    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override {
        if (fail_textures > 0) {
            --fail_textures;
            return nullptr;
        }
        ++textures_created;
        return std::make_unique<FakeTexture>(desc);
    }

    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc&) override {
        return nullptr;
    }
    std::unique_ptr<rhi::Pipeline> create_pipeline(const rhi::PipelineDesc&) override {
        return nullptr;
    }
    std::unique_ptr<rhi::RenderTarget> create_render_target(const rhi::RenderTargetDesc&) override {
        return nullptr;
    }
    std::unique_ptr<rhi::CommandList> create_command_list() override {
        return nullptr;
    }
    [[nodiscard]] std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
    create_swapchain(const rhi::SwapchainDesc&) override {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    void wait_idle() override {}

    [[nodiscard]] rhi::DeviceStatus status() const override {
        return device_status;
    }
    void set_status(rhi::DeviceStatus status) {
        device_status = status;
    }

    std::uint32_t textures_created = 0;
    std::uint32_t fail_textures = 0;
    rhi::DeviceStatus device_status = rhi::DeviceStatus::Ready;
};

// A small triangle inside a 32x32 box: geometry is irrelevant to the fake rasterizer but keeps the
// ThorVG-backed content check meaningful.
[[nodiscard]] inline path::PathShape triangle_path() {
    path::Path path;
    path.segments.push_back({path::SegmentKind::move_to, {4.0f, 4.0f}, {}, {}});
    path.segments.push_back({path::SegmentKind::line_to, {28.0f, 4.0f}, {}, {}});
    path.segments.push_back({path::SegmentKind::line_to, {16.0f, 28.0f}, {}, {}});
    path.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    path::PaintStyle paint;
    paint.fill = path::Rgba8{200, 40, 90, 255};
    return path::PathShape{std::move(path), paint};
}

[[nodiscard]] inline const FakeTexture* fake_texture(const path::CachedPathTexture& cached) {
    return dynamic_cast<const FakeTexture*>(cached.texture);
}

} // namespace tessera::test::path_texture
