// SPDX-License-Identifier: MIT
//
// `CommandList::copy_buffer_to_texture` (RHI 1.17, ADR-072) on Vulkan, read back through the
// pipeline: bytes from a staging buffer, from a host-written storage buffer and from a compute
// dispatch recorded earlier in the same list land in the right rectangle with the right bytes (the
// row pitch and buffer offset honoured, every texel outside untouched), a later copy wins over an
// earlier one, and misuse is refused before it reaches the driver. The texture is drawn 1:1 with a
// Nearest filter, byte-exact at identity scale (#687). The compute case needs `supports_compute()`
// and skips alone without it. Skips cleanly when no Vulkan device is available.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#if defined(TESSERA_HAS_COPY_PROBE)
alignas(4) static const std::uint32_t kProbeSpv[] =
#include "compute_probe_comp_spv.h"
    ;
#endif

namespace {

namespace rhi = tessera::rendering::rhi;
namespace gpu = tessera::rendering::gpu;

constexpr std::uint32_t kWidth = 32;
constexpr std::uint32_t kHeight = 24;

[[nodiscard]] bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "buffer_texture_copy: " << message << "\n";
    }
    return condition;
}

template<typename Exception, typename F> [[nodiscard]] bool throws_as(F&& call) {
    try {
        call();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// Opaque noise: any wrong byte, offset or row shows.
[[nodiscard]] std::vector<std::uint8_t> noise(std::size_t pixels, std::uint32_t seed) {
    std::vector<std::uint8_t> bytes(pixels * 4U);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        seed = seed * 1664525U + 1013904223U;
        bytes[i] = (i % 4U == 3U) ? std::uint8_t{255} : static_cast<std::uint8_t>(seed >> 24);
    }
    return bytes;
}

// Draws `texture` 1:1 into a kWidth x kHeight target and returns the readback.
[[nodiscard]] std::vector<std::uint8_t> read_back(rhi::Device& device, rhi::Texture& texture) {
    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {kWidth, kHeight};
    target_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    target_desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    auto target = device.create_render_target(target_desc);

    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    pipeline_desc.color_format = target_desc.format;
    auto pipeline = device.create_pipeline(pipeline_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device.create_texture(white_desc);

    gpu::Batched2DRenderer renderer(device, *pipeline, *white);
    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, target_desc);
    renderer.push_texture({0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)},
                          texture, {0.0f, 0.0f, 1.0f, 1.0f}, rhi::rgba8(255, 255, 255));
    renderer.end();
    commands->submit();
    device.wait_idle();

    const std::size_t bytes = static_cast<std::size_t>(kWidth) * kHeight * 4U;
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto copy = device.create_command_list();
    copy->copy_target_to_buffer(*target, *readback);
    copy->submit();
    device.wait_idle();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

[[nodiscard]] std::unique_ptr<rhi::Texture> make_texture(rhi::Device& device,
                                                         const std::vector<std::uint8_t>& pixels) {
    rhi::TextureDesc desc;
    desc.extent = {kWidth, kHeight};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.pixels = pixels.data();
    desc.row_pitch = static_cast<std::size_t>(kWidth) * 4U;
    desc.filter = rhi::Filter::Nearest;
    auto texture = device.create_texture(desc);
    device.wait_idle();
    return texture;
}

// The rows of `region` out of the kWidth-wide image `from`, `pitch` bytes apart, after `offset`
// bytes of 0xEE filler: the buffer layout the copy reads. The filler and the row padding are
// distinctive so a copy that reads the wrong bytes shows.
[[nodiscard]] std::vector<std::uint8_t> pack(const std::vector<std::uint8_t>& from,
                                             const rhi::TextureRegion& region, std::size_t offset,
                                             std::size_t pitch) {
    std::vector<std::uint8_t> bytes(offset + pitch * region.height, 0xEE);
    for (std::uint32_t row = 0; row < region.height; ++row) {
        std::memcpy(bytes.data() + offset + static_cast<std::size_t>(row) * pitch,
                    from.data() +
                        (static_cast<std::size_t>(region.y + row) * kWidth + region.x) * 4U,
                    static_cast<std::size_t>(region.width) * 4U);
    }
    return bytes;
}

void paste(std::vector<std::uint8_t>& into, const std::vector<std::uint8_t>& from,
           const rhi::TextureRegion& region) {
    for (std::uint32_t row = 0; row < region.height; ++row) {
        const std::size_t index =
            (static_cast<std::size_t>(region.y + row) * kWidth + region.x) * 4U;
        std::memcpy(into.data() + index, from.data() + index,
                    static_cast<std::size_t>(region.width) * 4U);
    }
}

[[nodiscard]] rhi::BufferTextureCopy copy_of(rhi::TextureRegion region, std::size_t offset,
                                             std::size_t pitch) {
    rhi::BufferTextureCopy copy;
    copy.region = region;
    copy.buffer_offset = offset;
    copy.buffer_row_pitch = pitch;
    return copy;
}

bool check_sources(rhi::Device& device) {
    const auto base = noise(static_cast<std::size_t>(kWidth) * kHeight, 1U);
    const auto patch = noise(static_cast<std::size_t>(kWidth) * kHeight, 2U);
    bool ok = true;

    // (a) Staging, whole texture, default pitch: 32 texels * 4 = 128 bytes pad to 256.
    {
        const rhi::TextureRegion whole{0, 0, kWidth, kHeight};
        const std::size_t pitch =
            rhi::aligned_texture_copy_pitch(kWidth, rhi::TextureFormat::R8G8B8A8Unorm);
        ok = expect(pitch == 256, "a 32-texel RGBA8 row pads to 256 bytes") && ok;
        const auto bytes = pack(patch, whole, 0, pitch);
        auto staging = device.create_buffer({bytes.size(), rhi::BufferUsage::Staging});
        staging->write(bytes.data(), bytes.size());
        auto texture = make_texture(device, base);
        auto list = device.create_command_list();
        list->copy_buffer_to_texture(*staging, *texture, copy_of(whole, 0, 0));
        list->submit();
        ok = expect(read_back(device, *texture) == patch, "a whole-texture staging copy") && ok;
    }

    // (b) Storage, a sub-rectangle at a buffer offset with a padded pitch (512, wider than needed).
    {
        const rhi::TextureRegion region{5, 3, 11, 7};
        const auto bytes = pack(patch, region, 64, 512);
        auto storage = device.create_buffer({bytes.size(), rhi::BufferUsage::Storage});
        storage->write(bytes.data(), bytes.size());
        auto texture = make_texture(device, base);
        auto list = device.create_command_list();
        list->copy_buffer_to_texture(*storage, *texture, copy_of(region, 64, 512));
        list->submit();
        auto expected = base;
        paste(expected, patch, region);
        ok = expect(read_back(device, *texture) == expected,
                    "an offset, padded-pitch storage copy changes exactly the rectangle") &&
             ok;
    }

    // (c) Corners and a one-texel region, and a later copy over an earlier one: the last wins.
    {
        auto texture = make_texture(device, base);
        auto expected = base;
        const auto other = noise(static_cast<std::size_t>(kWidth) * kHeight, 3U);
        auto list = device.create_command_list();
        std::vector<std::unique_ptr<rhi::Buffer>> keep;
        const std::array<std::pair<const std::vector<std::uint8_t>*, rhi::TextureRegion>, 4> steps{
            {{&patch, {0, 0, 16, 12}},
             {&other, {8, 6, 16, 12}},
             {&patch, {kWidth - 1, kHeight - 1, 1, 1}},
             {&other, {0, 0, 1, 1}}}};
        for (const auto& [source, region] : steps) {
            const auto bytes = pack(*source, region, 0, 256);
            keep.push_back(device.create_buffer({bytes.size(), rhi::BufferUsage::Staging}));
            keep.back()->write(bytes.data(), bytes.size());
            list->copy_buffer_to_texture(*keep.back(), *texture, copy_of(region, 0, 0));
            paste(expected, *source, region);
        }
        list->submit();
        ok = expect(read_back(device, *texture) == expected,
                    "overlapping and corner copies apply in recording order") &&
             ok;
    }

    // (d) Misuse is refused, and the texture is untouched by a refused or empty copy.
    {
        auto staging = device.create_buffer({256 * kHeight, rhi::BufferUsage::Staging});
        auto vertex = device.create_buffer({256 * kHeight, rhi::BufferUsage::Vertex});
        auto texture = make_texture(device, base);
        auto list = device.create_command_list();
        ok =
            expect(throws_as<std::invalid_argument>([&] {
                       list->copy_buffer_to_texture(*vertex, *texture, copy_of({0, 0, 4, 4}, 0, 0));
                   }),
                   "a vertex buffer is refused") &&
            ok;
        ok = expect(throws_as<std::out_of_range>([&] {
                        list->copy_buffer_to_texture(*staging, *texture,
                                                     copy_of({kWidth - 1, 0, 2, 2}, 0, 0));
                    }),
                    "a region past the edge is out_of_range") &&
             ok;
        ok = expect(throws_as<std::invalid_argument>([&] {
                        list->copy_buffer_to_texture(*staging, *texture,
                                                     copy_of({0, 0, 4, 4}, 0, 260));
                    }),
                    "a pitch that is not a multiple of 256 is refused") &&
             ok;
        ok = expect(throws_as<std::invalid_argument>([&] {
                        list->copy_buffer_to_texture(*staging, *texture,
                                                     copy_of({0, 0, 4, 4}, 2, 0));
                    }),
                    "an offset that is not a multiple of 4 is refused") &&
             ok;
        list->copy_buffer_to_texture(*staging, *texture, copy_of({4, 4, 0, 3}, 0, 0));
        list->submit();
        ok = expect(read_back(device, *texture) == base,
                    "a refused or empty copy leaves the texture unchanged") &&
             ok;
    }
    return ok;
}

#if defined(TESSERA_HAS_COPY_PROBE)
// A dispatch writes the pixels (out = in * 1 + 1, per 32-bit word) and the copy that follows in the
// same list reads them: the barrier the backend records between them is what this proves.
bool check_compute_fed(rhi::Device& device) {
    struct Push {
        std::uint32_t mul;
        std::uint32_t count;
    };
    const std::size_t pitch = 256;
    const std::size_t bytes = pitch * kHeight;
    const std::uint32_t words = static_cast<std::uint32_t>(bytes / 4U);
    const rhi::TextureRegion whole{0, 0, kWidth, kHeight};
    const auto source = noise(static_cast<std::size_t>(kWidth) * kHeight, 5U);
    const auto packed = pack(source, whole, 0, pitch);

    rhi::ComputePipelineDesc compute;
    compute.spirv = kProbeSpv;
    compute.spirv_size = sizeof(kProbeSpv);
    compute.storage_buffer_count = 2;
    compute.push_constant_size = sizeof(Push);
    const auto pipeline = device.create_compute_pipeline(compute);

    auto input = device.create_buffer({bytes, rhi::BufferUsage::Storage});
    input->write(packed.data(), bytes);
    rhi::BufferDesc out_desc{bytes, rhi::BufferUsage::Storage};
    out_desc.gpu_read_write = true;
    auto output = device.create_buffer(out_desc);

    const auto base = noise(static_cast<std::size_t>(kWidth) * kHeight, 4U);
    auto texture = make_texture(device, base);
    auto list = device.create_command_list();
    list->bind_compute_pipeline(*pipeline);
    list->bind_compute_buffer(0, *output);
    list->bind_compute_buffer(1, *input);
    const Push push{1, words};
    list->compute_push_constants(&push, sizeof(push));
    list->dispatch((words + 63U) / 64U, 1, 1);
    list->copy_buffer_to_texture(*output, *texture, copy_of(whole, 0, pitch));
    list->submit();

    // The shader adds one to each word; the expected texels are the packed words plus one, with
    // the carry a 32-bit add has (the texel bytes are little-endian words on every target).
    std::vector<std::uint8_t> expected(base.size());
    for (std::uint32_t row = 0; row < kHeight; ++row) {
        for (std::uint32_t column = 0; column < kWidth; ++column) {
            std::uint32_t word = 0;
            std::memcpy(&word, packed.data() + row * pitch + static_cast<std::size_t>(column) * 4U,
                        4);
            ++word;
            std::memcpy(expected.data() + (static_cast<std::size_t>(row) * kWidth + column) * 4U,
                        &word, 4);
        }
    }
    return expect(read_back(device, *texture) == expected,
                  "a compute dispatch's output reaches the texture through the copy");
}
#endif

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    device_desc.validation = true;
    std::unique_ptr<rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "buffer_texture_copy: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    try {
        if (!expect(device->supports_buffer_texture_copy(),
                    "the Vulkan backend reports the buffer-to-texture copy") ||
            !check_sources(*device)) {
            return 1;
        }
#if defined(TESSERA_HAS_COPY_PROBE)
        if (device->supports_compute()) {
            if (!check_compute_fed(*device)) {
                return 1;
            }
        } else {
            std::cout << "buffer_texture_copy: compute case skipped (no compute)\n";
        }
#endif
    } catch (const std::exception& error) {
        std::cerr << "buffer_texture_copy: unexpected exception: " << error.what() << "\n";
        return 3;
    }
    std::cout << "buffer_texture_copy: ok\n";
    return 0;
}
