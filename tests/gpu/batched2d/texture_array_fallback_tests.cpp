// SPDX-License-Identifier: MIT
//
// The texture-array fallback contract (RHI 1.7, issue #480 part 2). Descriptor indexing is optional
// on Vulkan and absent on WebGPU, so the capability is the only authority and a device without it
// must refuse the array path loudly instead of recording a draw that samples the wrong image.
//
// Runs on the null backend, which reports no texture array: this pins the documented fallback that
// every non-supporting device takes. The array path itself is pinned by
// `texture_array_gpu_tests` on a device that has the capability.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::create_null_device;
using tessera::rendering::rhi::rgba8;

constexpr std::uint32_t kSize = 64;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "texture_array_fallback: " << message << "\n";
    }
    return condition;
}

// The capability is the only authority, and the null backend has none.
int check_capability() {
    auto device = create_null_device({});
    if (!expect(device->texture_array_capacity() == 0,
                "a device without descriptor indexing reports capacity 0")) {
        return 1;
    }
    return 0;
}

// Asking for the array pipeline on a device without the capability is a named error, not a silent
// fallback: a caller that ignored the capability would otherwise draw with a layout mismatch.
int check_pipeline_refused() {
    auto device = create_null_device({});
    auto target = device->create_render_target({{kSize, kSize}});
    auto white = device->create_texture({{1, 1}});
    auto pipeline = device->create_pipeline({});
    Batched2DRenderer renderer(*device, *pipeline, *white);

    bool threw = false;
    try {
        renderer.set_texture_array_pipeline(pipeline.get());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "set_texture_array_pipeline refuses a device without the capability") ||
        !expect(!renderer.texture_array_supported(),
                "the refused pipeline leaves the renderer on the one-set path")) {
        return 1;
    }
    // Clearing it is always allowed, so a caller can drop the path unconditionally.
    renderer.set_texture_array_pipeline(nullptr);
    return 0;
}

// bind_texture_array on a device without the capability is a named error too, so a renderer bug
// surfaces at the bind instead of as a validation error on a real backend.
int check_bind_refused() {
    auto device = create_null_device({});
    auto target = device->create_render_target({{kSize, kSize}});
    auto white = device->create_texture({{1, 1}});
    auto other = device->create_texture({{2, 2}});
    auto pipeline = device->create_pipeline({});
    auto commands = device->create_command_list();

    commands->begin_render_pass(*target, {{kSize, kSize}});
    commands->bind_pipeline(*pipeline);
    const std::vector<const tessera::rendering::rhi::Texture*> textures{white.get(), other.get()};
    bool threw = false;
    try {
        commands->bind_texture_array(0, textures);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    commands->end_render_pass();
    commands->submit();
    if (!expect(threw, "bind_texture_array refuses a device without the capability")) {
        return 1;
    }
    return 0;
}

// An empty array is a no-op on every backend, so a caller that resolves no textures this batch does
// not have to special-case the call.
int check_empty_array() {
    auto device = create_null_device({});
    auto target = device->create_render_target({{kSize, kSize}});
    auto white = device->create_texture({{1, 1}});
    auto pipeline = device->create_pipeline({});
    auto commands = device->create_command_list();

    commands->begin_render_pass(*target, {{kSize, kSize}});
    commands->bind_pipeline(*pipeline);
    bool threw = false;
    try {
        commands->bind_texture_array(0, {});
    } catch (const std::exception&) {
        threw = true;
    }
    commands->end_render_pass();
    commands->submit();
    if (!expect(!threw, "an empty texture array is a no-op, not an error")) {
        return 1;
    }
    return 0;
}

// The one-set-per-texture path is unchanged: a texture change still ends the batch, which is what
// the fallback device keeps doing.
int check_fallback_still_splits() {
    auto device = create_null_device({});
    auto target = device->create_render_target({{kSize, kSize}});
    auto white = device->create_texture({{1, 1}});
    auto other = device->create_texture({{2, 2}});
    auto pipeline = device->create_pipeline({});
    auto commands = device->create_command_list();
    Batched2DRenderer renderer(*device, *pipeline, *white);

    renderer.begin(*commands, *target, {{kSize, kSize}});
    renderer.push_rect({0, 0, 8, 8}, rgba8(255, 0, 0));
    renderer.push_texture({8, 0, 8, 8}, *other, {0, 0, 1, 1}, rgba8(255, 255, 255));
    renderer.push_rect({16, 0, 8, 8}, rgba8(0, 255, 0));
    renderer.end();
    commands->submit();

    if (!expect(renderer.batch_split_stats().texture == 2,
                "the fallback still splits on every texture change") ||
        !expect(renderer.draw_calls() == 3, "the fallback records one draw per batch")) {
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    const int results[] = {check_capability(), check_pipeline_refused(), check_bind_refused(),
                           check_empty_array(), check_fallback_still_splits()};
    for (const int result : results) {
        if (result != 0) {
            return result;
        }
    }
    std::cout << "texture_array_fallback: ok\n";
    return 0;
}
