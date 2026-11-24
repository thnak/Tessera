// SPDX-License-Identifier: MIT
//
// RHI 1.17 `CommandList::copy_buffer_to_texture` (ADR-072) on the null backend: the whole
// validation matrix (the shared `checked_buffer_texture_copy` plus the usage, sample-count, mip and
// render-pass refusals), the recorded request and the command order. The null backend is the
// validating reference: the Vulkan backend makes the same refusals
// (tests/gpu/rhi/buffer_texture_copy_tests.cpp reads the pixels back on a real device).

#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

namespace {

using namespace tessera::rendering;
namespace rhi = tessera::rendering::rhi;

static_assert(rhi::aligned_texture_copy_pitch(1, rhi::TextureFormat::R8G8B8A8Unorm) == 256);
static_assert(rhi::aligned_texture_copy_pitch(64, rhi::TextureFormat::R8G8B8A8Unorm) == 256);
static_assert(rhi::aligned_texture_copy_pitch(65, rhi::TextureFormat::R8G8B8A8Unorm) == 512);
static_assert(rhi::aligned_texture_copy_pitch(3840, rhi::TextureFormat::R8G8B8A8Unorm) == 15360);
static_assert(rhi::aligned_texture_copy_pitch(256, rhi::TextureFormat::R8Unorm) == 256);
static_assert(rhi::aligned_texture_copy_pitch(257, rhi::TextureFormat::R8Unorm) == 512);

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "buffer_texture_copy_null: " << message << "\n";
    }
    return condition;
}

template<typename Exception, typename F> bool throws_as(F&& call) {
    try {
        call();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

constexpr std::uint32_t kWidth = 100;
constexpr std::uint32_t kHeight = 40;
constexpr std::size_t kPitch = 512; // 100 texels * 4 bytes = 400, padded to a multiple of 256

struct Fixture {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Texture> texture;
    std::unique_ptr<rhi::Buffer> staging;
    std::unique_ptr<rhi::CommandList> list;

    Fixture() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::Null;
        device = null::create_null_device(desc);
        rhi::TextureDesc texture_desc;
        texture_desc.extent = {kWidth, kHeight};
        texture = device->create_texture(texture_desc);
        staging = device->create_buffer({kPitch * kHeight, rhi::BufferUsage::Staging});
        list = device->create_command_list();
    }

    [[nodiscard]] null::NullCommandList& null_list() const {
        return static_cast<null::NullCommandList&>(*list);
    }
};

rhi::BufferTextureCopy copy_of(rhi::TextureRegion region, std::size_t offset = 0,
                               std::size_t pitch = 0) {
    rhi::BufferTextureCopy copy;
    copy.region = region;
    copy.buffer_offset = offset;
    copy.buffer_row_pitch = pitch;
    return copy;
}

bool check_capability_and_record() {
    Fixture f;
    bool ok = expect(f.device->supports_buffer_texture_copy(),
                     "the null backend reports the buffer-to-texture copy");
    // A whole-texture copy with the default pitch (the 100-texel row pads to 512).
    f.list->copy_buffer_to_texture(*f.staging, *f.texture, copy_of({0, 0, kWidth, kHeight}));
    // A sub-rectangle at an offset with an explicit pitch.
    f.list->copy_buffer_to_texture(*f.staging, *f.texture, copy_of({10, 5, 20, 8}, 1024, 512));
    f.list->submit();
    const auto& stats = f.null_list().stats();
    ok = expect(stats.buffer_texture_copies.size() == 2, "both copies are recorded") && ok;
    if (stats.buffer_texture_copies.size() == 2) {
        const auto& whole = stats.buffer_texture_copies[0];
        ok = expect(whole.buffer_row_pitch == kPitch && whole.region.width == kWidth &&
                        whole.buffer_usage == rhi::BufferUsage::Staging &&
                        whole.texture_extent.width == kWidth,
                    "the default pitch is the aligned one and the identity is recorded") &&
             ok;
        const auto& part = stats.buffer_texture_copies[1];
        ok = expect(part.buffer_offset == 1024 && part.region.x == 10 && part.region.y == 5,
                    "an offset and a sub-rectangle are recorded") &&
             ok;
    }
    ok = expect(stats.command_order.size() == 3 &&
                    stats.command_order[0] == null::NullCommandKind::CopyBufferToTexture &&
                    stats.command_order[1] == null::NullCommandKind::CopyBufferToTexture &&
                    stats.command_order[2] == null::NullCommandKind::Submit,
                "the copies are logged in order before the submit") &&
         ok;
    return ok;
}

bool check_sources() {
    Fixture f;
    bool ok = true;
    // Staging and Storage (host-writable or device-local) are sources; nothing else is.
    const auto storage = f.device->create_buffer({kPitch * kHeight, rhi::BufferUsage::Storage});
    f.list->copy_buffer_to_texture(*storage, *f.texture, copy_of({0, 0, 8, 8}));
    for (const rhi::BufferUsage usage : {rhi::BufferUsage::Vertex, rhi::BufferUsage::Index,
                                         rhi::BufferUsage::Uniform, rhi::BufferUsage::Readback}) {
        const auto buffer = f.device->create_buffer({kPitch * kHeight, usage});
        ok = expect(throws_as<std::invalid_argument>([&] {
                        f.list->copy_buffer_to_texture(*buffer, *f.texture, copy_of({0, 0, 8, 8}));
                    }),
                    "a buffer that is not Staging or Storage is refused") &&
             ok;
    }
    return ok;
}

bool check_destination() {
    Fixture f;
    bool ok = true;
    rhi::TextureDesc multisampled;
    multisampled.extent = {kWidth, kHeight};
    multisampled.sample_count = 4;
    const auto msaa = f.device->create_texture(multisampled);
    ok = expect(throws_as<std::invalid_argument>([&] {
                    f.list->copy_buffer_to_texture(*f.staging, *msaa, copy_of({0, 0, 8, 8}));
                }),
                "a multisampled texture is refused") &&
         ok;
    // A mip chain needs the base level uploaded (pixels), and is immutable.
    const std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4U, 7);
    rhi::TextureDesc mipped;
    mipped.extent = {kWidth, kHeight};
    mipped.pixels = pixels.data();
    mipped.levels = 3;
    const auto mips = f.device->create_texture(mipped);
    ok = expect(throws_as<std::invalid_argument>([&] {
                    f.list->copy_buffer_to_texture(*f.staging, *mips, copy_of({0, 0, 8, 8}));
                }),
                "a mipmapped texture is refused") &&
         ok;
    // R8Unorm: one byte per texel, so a 100-texel row is 100 bytes and the pitch still pads to 256.
    rhi::TextureDesc single;
    single.extent = {kWidth, kHeight};
    single.format = rhi::TextureFormat::R8Unorm;
    const auto r8 = f.device->create_texture(single);
    f.list->copy_buffer_to_texture(*f.staging, *r8, copy_of({0, 0, kWidth, kHeight}));
    const auto& records = f.null_list().stats().buffer_texture_copies;
    ok = expect(!records.empty() && records.back().buffer_row_pitch == 256,
                "an R8Unorm row pads to the 256-byte alignment") &&
         ok;
    return ok;
}

bool check_geometry() {
    Fixture f;
    bool ok = true;
    const auto refuses = [&](rhi::BufferTextureCopy copy, const char* what) {
        return expect(throws_as<std::invalid_argument>(
                          [&] { f.list->copy_buffer_to_texture(*f.staging, *f.texture, copy); }),
                      what);
    };
    ok = expect(throws_as<std::out_of_range>([&] {
                    f.list->copy_buffer_to_texture(*f.staging, *f.texture, copy_of({95, 0, 10, 4}));
                }),
                "a region past the right edge is out_of_range") &&
         ok;
    ok = expect(throws_as<std::out_of_range>([&] {
                    f.list->copy_buffer_to_texture(*f.staging, *f.texture, copy_of({0, 38, 4, 4}));
                }),
                "a region past the bottom edge is out_of_range") &&
         ok;
    ok = refuses(copy_of({0, 0, 8, 8}, 0, 260), "a pitch that is not a multiple of 256") && ok;
    ok = refuses(copy_of({0, 0, kWidth, 2}, 0, 256), "a pitch shorter than one row") && ok;
    ok = refuses(copy_of({0, 0, 8, 8}, 2), "an offset that is not a multiple of 4") && ok;
    // The buffer holds kPitch * kHeight bytes: rows 0..39 at pitch 512 need 39 * 512 + 400 = 20368
    // bytes of 20480.
    ok = refuses(copy_of({0, 0, kWidth, kHeight}, 116, kPitch),
                 "a copy that reads past the buffer") &&
         ok;
    ok = refuses(copy_of({0, 0, 8, 2}, f.staging->size() + 4), "an offset past the end") && ok;
    // A pitch near SIZE_MAX must be refused, not wrap around into a small requirement.
    const std::size_t huge = (std::numeric_limits<std::size_t>::max() / 256U) * 256U;
    ok =
        refuses(copy_of({0, 0, 8, 2}, 0, huge), "a pitch that would overflow the size check") && ok;
    // The exact fit is valid: offset 4, 2 rows at pitch 512 end at 4 + 512 + 400.
    const auto exact = f.device->create_buffer(
        {static_cast<std::size_t>(4 + 512 + 400), rhi::BufferUsage::Staging});
    f.list->copy_buffer_to_texture(*exact, *f.texture, copy_of({0, 0, kWidth, 2}, 4, 512));
    // An empty region is valid and copies nothing, whatever else it names.
    const std::size_t before = f.null_list().stats().buffer_texture_copies.size();
    f.list->copy_buffer_to_texture(*f.staging, *f.texture, copy_of({500, 500, 0, 9}, 3, 7));
    ok = expect(f.null_list().stats().buffer_texture_copies.size() == before,
                "an empty region records nothing") &&
         ok;
    return ok;
}

bool check_render_pass() {
    Fixture f;
    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {16, 16};
    const auto target = f.device->create_render_target(target_desc);
    f.list->begin_render_pass(*target, target_desc);
    bool ok =
        expect(throws_as<std::logic_error>([&] {
                   f.list->copy_buffer_to_texture(*f.staging, *f.texture, copy_of({0, 0, 8, 8}));
               }),
               "a copy inside a render pass is a logic_error");
    f.list->end_render_pass();
    f.list->copy_buffer_to_texture(*f.staging, *f.texture, copy_of({0, 0, 8, 8}));
    ok = expect(f.null_list().stats().buffer_texture_copies.size() == 1,
                "the same copy after the pass ends is recorded") &&
         ok;
    return ok;
}

} // namespace

int main() {
    try {
        const bool ok = check_capability_and_record() && check_sources() && check_destination() &&
                        check_geometry() && check_render_pass();
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "buffer_texture_copy_null: unexpected exception: " << error.what() << "\n";
        return 1;
    }
}
