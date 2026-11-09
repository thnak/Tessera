// SPDX-License-Identifier: MIT
//
// A create_* call that fails part-way must release every Vulkan object it already made. Failures
// are forced after partial progress (descriptor pool exhausted after the image, view and sampler
// exist; a broken fragment shader after the vertex module exists). On device destruction the
// validation layer reports anything left behind as "... leaked objects that have not been
// destroyed", which this test looks for in the captured diagnostic stream. Other validation chatter
// caused by the deliberately bad input is expected and ignored.

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using tessera::rendering::rhi::Device;

constexpr std::size_t kMaxTextures = 1100; // more than the 1024-set descriptor pool holds

// Routes the device's "[vulkan] ..." diagnostics into a string for the lifetime of the object.
class CerrCapture {
public:
    CerrCapture()
        : previous_(std::cerr.rdbuf(buffer_.rdbuf())) {}
    ~CerrCapture() {
        std::cerr.rdbuf(previous_);
    }
    CerrCapture(const CerrCapture&) = delete;
    CerrCapture& operator=(const CerrCapture&) = delete;

    [[nodiscard]] std::string text() const {
        return buffer_.str();
    }

private:
    std::ostringstream buffer_;
    std::streambuf* previous_;
};

enum class Outcome { Skipped, Passed, Failed };

// Runs `body` on a fresh validation-enabled device, destroys the device, and fails if the
// validation layer reported a leaked object. `body` must report that it hit the forced failure.
//
// `required` says whether the failure is deterministic everywhere. The descriptor-pool case is not:
// some drivers (lavapipe) do not enforce the pool's set limit, so there the failure never happens
// and the part only proves that a run without failure leaks nothing.
Outcome run_part(const char* name, const std::function<bool(Device&)>& body, bool required = true) {
    CerrCapture capture;
    tessera::rendering::rhi::DeviceDesc desc;
    desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(desc);
    } catch (const std::exception&) {
        return Outcome::Skipped;
    }

    const bool hit_failure = body(*device);
    device->wait_idle();
    device.reset();

    // Evidence goes to std::clog, which is not captured.
    const std::string text = capture.text();
    // The layer reports "vkDestroyDevice(): VkDevice ... has N leaked objects that have not been
    // destroyed" followed by the object list.
    const bool leaked = text.find("leaked objects") != std::string::npos;
    if (!hit_failure) {
        std::clog << "vulkan_create_failure[" << name << "]: the forced failure did not happen"
                  << (required ? "\n" : " (driver does not enforce it; not required)\n");
        if (required) {
            return Outcome::Failed;
        }
    }
    if (leaked) {
        std::clog << "vulkan_create_failure[" << name << "]: leaked Vulkan objects:\n" << text;
        return Outcome::Failed;
    }
    return Outcome::Passed;
}

bool exhaust_descriptor_pool(Device& device) {
    std::vector<std::unique_ptr<tessera::rendering::rhi::Texture>> textures;
    try {
        for (std::size_t i = 0; i < kMaxTextures; ++i) {
            tessera::rendering::rhi::TextureDesc desc;
            desc.extent = {1, 1};
            textures.push_back(device.create_texture(desc));
        }
    } catch (const std::exception&) {
        return true; // create_texture threw after the image, view and sampler were created
    }
    return false;
}

bool fail_texture_upload(Device& device) {
    // The image, view, sampler and descriptor set exist and the texture object owns them when the
    // staging buffer is requested; a row pitch no device can allocate makes that request fail.
    const std::uint8_t pixel[4] = {0, 0, 0, 0};
    tessera::rendering::rhi::TextureDesc desc;
    desc.extent = {1, 1};
    desc.pixels = pixel;
    desc.row_pitch = std::size_t{1} << 40; // 1 TiB
    try {
        const auto texture = device.create_texture(desc);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

bool break_fragment_shader(Device& device) {
    static const std::array<std::uint32_t, 4> kNotSpirv = {0, 0, 0, 0};
    tessera::rendering::rhi::PipelineDesc desc =
        tessera::rendering::vulkan::batched2d_pipeline_desc();
    desc.fragment_spirv = kNotSpirv.data();
    desc.fragment_spirv_size = sizeof(kNotSpirv);
    try {
        const auto pipeline = device.create_pipeline(desc);
    } catch (const std::exception&) {
        return true; // vkCreateShaderModule(fragment) failed after the vertex module existed
    }
    return false;
}

} // namespace

int main() {
    const Outcome upload = run_part("texture-upload", fail_texture_upload);
    if (upload == Outcome::Skipped) {
        std::cout << "vulkan_create_failure: skipped (Vulkan device unavailable)\n";
        return 0;
    }
    if (upload == Outcome::Failed) {
        return 1;
    }
    if (run_part("texture-pool", exhaust_descriptor_pool, false) == Outcome::Failed) {
        return 3;
    }
    if (run_part("pipeline", break_fragment_shader) == Outcome::Failed) {
        return 2;
    }
    std::cout << "vulkan_create_failure: ok\n";
    return 0;
}
