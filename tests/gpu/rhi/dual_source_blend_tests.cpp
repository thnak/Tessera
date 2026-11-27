// SPDX-License-Identifier: MIT
//
// RHI dual-source blend contract (RHI 1.5, issue #471 slice 1).
//
// Three facts, one per backend that can answer them on this host:
//  - null records the requested blend mode on the created pipeline, so the mode is assertable on
//  any
//    machine (`Device::supports_dual_source_blend()` true; nothing rasterizes, so nothing can
//    blend);
//  - Vulkan creates a `BlendMode::DualSource` pipeline when the physical device reports
//    `dualSrcBlend`, and rejects the request when it does not. The device is real: pipeline
//    creation is where the SRC1 blend factors meet the enabled feature, so this is the proof the
//    state is legal. A device without the feature is a skip with the reason printed, never a pass;
//  - WebGPU answers false and `blend_state()` refuses the mode instead of mapping it to "no blend"
//    (pinned in tests/unit/webgpu_mapping_tests.cpp, the host-buildable half of that backend; the
//    Emscripten pipeline half cannot be built here).
//
// No frame changes: nothing here is on a renderer, window or paint path.

#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

#if defined(TESSERA_TEST_HAS_VULKAN)
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#endif

namespace {

using tessera::rendering::rhi::Backend;
using tessera::rendering::rhi::BlendMode;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::DeviceDesc;
using tessera::rendering::rhi::PipelineDesc;
using tessera::rendering::rhi::TextureFormat;
namespace rhi = tessera::rendering::rhi;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "dual_source_blend: " << message << "\n";
    }
    return condition;
}

// The null backend records the request instead of blending, so the created pipeline is the
// evidence: a mode that was silently dropped would be indistinguishable from Alpha here.
bool check_null_records_the_mode() {
    auto device = tessera::rendering::null::create_null_device(DeviceDesc{});
    bool ok = expect(device->supports_dual_source_blend(),
                     "null reports dual-source blending as supported (it records, never blends)");

    tessera::rendering::rhi::PipelineDesc desc;
    desc.blend = BlendMode::DualSource;
    auto pipeline = device->create_pipeline(desc);
    const auto* recorded =
        dynamic_cast<const tessera::rendering::null::NullPipeline*>(pipeline.get());
    ok = expect(recorded != nullptr, "null create_pipeline returns the null pipeline record") && ok;
    if (recorded != nullptr) {
        ok = expect(recorded->blend_mode() == BlendMode::DualSource,
                    "null records BlendMode::DualSource on the pipeline") &&
             ok;
    }

    // None/Alpha are untouched by the new mode: the recording reflects the request exactly.
    tessera::rendering::rhi::PipelineDesc none_desc;
    none_desc.blend = BlendMode::None;
    auto none = device->create_pipeline(none_desc);
    tessera::rendering::rhi::PipelineDesc alpha_desc;
    alpha_desc.blend = BlendMode::Alpha;
    auto alpha = device->create_pipeline(alpha_desc);
    const auto* none_recorded =
        dynamic_cast<const tessera::rendering::null::NullPipeline*>(none.get());
    const auto* alpha_recorded =
        dynamic_cast<const tessera::rendering::null::NullPipeline*>(alpha.get());
    ok = expect(none_recorded != nullptr && none_recorded->blend_mode() == BlendMode::None,
                "null still records BlendMode::None unchanged") &&
         ok;
    ok = expect(alpha_recorded != nullptr && alpha_recorded->blend_mode() == BlendMode::Alpha,
                "null still records BlendMode::Alpha unchanged") &&
         ok;
    // RHI 1.19: the layer mask step is recorded as requested too.
    tessera::rendering::rhi::PipelineDesc mask_desc;
    mask_desc.blend = BlendMode::DestinationIn;
    auto mask = device->create_pipeline(mask_desc);
    const auto* mask_recorded =
        dynamic_cast<const tessera::rendering::null::NullPipeline*>(mask.get());
    ok = expect(mask_recorded != nullptr && mask_recorded->blend_mode() == BlendMode::DestinationIn,
                "null records BlendMode::DestinationIn on the pipeline") &&
         ok;
    return ok;
}

#if defined(TESSERA_TEST_HAS_VULKAN)
// Returns 0 when the check ran or skipped cleanly, 1 when it failed.
int check_vulkan_creates_or_rejects() {
    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "dual_source_blend: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    const rhi::PipelineDesc lcd_desc =
        tessera::rendering::vulkan::batched2d_lcd_pipeline_desc(TextureFormat::R8G8B8A8Unorm, 1);
    if (!device->supports_dual_source_blend()) {
        // A device without dualSrcBlend must refuse, not map to another blend. The capability
        // answer and the rejection are the whole contract here, so asserting it is not a false
        // pass; the feature itself is what this run cannot exercise.
        bool rejected = false;
        try {
            (void)device->create_pipeline(lcd_desc);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        if (!expect(rejected,
                    "a device without dualSrcBlend rejects a DualSource pipeline request")) {
            std::cerr << "dual_source_blend: failed\n";
            return 1;
        }
        std::cout << "dual_source_blend: skipped (device reports no dualSrcBlend; the request was "
                     "rejected as the contract requires)\n";
        return 0;
    }

    bool ok = expect(lcd_desc.blend == BlendMode::DualSource,
                     "the LCD pipeline descriptor requests BlendMode::DualSource");
    try {
        auto pipeline = device->create_pipeline(lcd_desc);
        ok = expect(pipeline != nullptr && pipeline->sample_count() == 1,
                    "Vulkan creates the DualSource pipeline and reports its sample count") &&
             ok;
    } catch (const std::exception& error) {
        std::cerr << "dual_source_blend: create_pipeline threw: " << error.what() << "\n";
        ok = false;
    }

    // The ordinary pipelines still create unchanged on a device that also supports dual source.
    try {
        auto alpha = device->create_pipeline(
            tessera::rendering::vulkan::batched2d_pipeline_desc(TextureFormat::R8G8B8A8Unorm, 1));
        ok = expect(alpha != nullptr, "the Alpha batched2d pipeline still creates") && ok;
    } catch (const std::exception& error) {
        std::cerr << "dual_source_blend: Alpha pipeline threw: " << error.what() << "\n";
        ok = false;
    }

    if (!ok) {
        std::cerr << "dual_source_blend: failed\n";
        return 1;
    }
    std::cout << "dual_source_blend: Vulkan created a DualSource pipeline\n";
    return 0;
}
#endif // TESSERA_TEST_HAS_VULKAN

} // namespace

int main() {
    if (!check_null_records_the_mode()) {
        std::cerr << "dual_source_blend: failed\n";
        return 1;
    }
#if defined(TESSERA_TEST_HAS_VULKAN)
    const int vulkan = check_vulkan_creates_or_rejects();
    if (vulkan != 0) {
        return vulkan;
    }
#else
    std::cout << "dual_source_blend: skipped (built without the Vulkan backend; null checks ran)\n";
#endif
    std::cout << "dual_source_blend: ok\n";
    return 0;
}
