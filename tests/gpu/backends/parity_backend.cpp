// SPDX-License-Identifier: MIT

#include "parity_backend.hpp"
#include "tessera/rendering/null/null_device.hpp"

#if defined(TESSERA_TEST_HAS_VULKAN)
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#endif

#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

namespace tessera::tests::gpu::backends {
namespace {

using namespace tessera::rendering;

std::string env_value(const char* name) {
#if defined(_MSC_VER)
    // std::getenv is deprecated by MSVC (/WX is on); _dupenv_s is the documented replacement and
    // also makes the missing-variable case explicit.
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
        return std::string();
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

bool is_lavapipe_manifest(std::string_view entry) noexcept {
    return entry.find("lvp_icd") != std::string_view::npos ||
           entry.find("lavapipe") != std::string_view::npos;
}

OpenedArm unavailable(Arm arm, std::string reason) {
    OpenedArm opened;
    opened.arm = arm;
    opened.name = std::string(arm_name(arm));
    opened.unavailable_reason = std::move(reason);
    return opened;
}

OpenedArm open_null() {
    OpenedArm opened;
    opened.arm = Arm::Null;
    opened.name = "null";
    opened.structure_only = true;
    opened.device = null::create_null_device(rhi::DeviceDesc{});
    if (opened.device == nullptr || opened.device->backend() != rhi::Backend::Null) {
        return unavailable(Arm::Null, "the null backend did not return a null device");
    }
    return opened;
}

OpenedArm open_lavapipe() {
    OpenedArm opened;
    opened.arm = Arm::Lavapipe;
    opened.name = "lavapipe";
#if defined(TESSERA_TEST_HAS_VULKAN)
    if (!process_selects_lavapipe()) {
        return unavailable(Arm::Lavapipe,
                           "the lavapipe software Vulkan driver is not selected (set "
                           "VK_ICD_FILENAMES to lvp_icd.*.json)");
    }
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::Vulkan;
    desc.validation = false;
    try {
        opened.device = vulkan::create_vulkan_device(desc);
    } catch (const std::exception&) {
        opened.device.reset();
        return unavailable(Arm::Lavapipe, "no lavapipe device could be created");
    }
    if (opened.device == nullptr || opened.device->backend() != rhi::Backend::Vulkan) {
        opened.device.reset();
        return unavailable(Arm::Lavapipe, "the Vulkan backend did not return a Vulkan device");
    }
#else
    return unavailable(Arm::Lavapipe, "the Vulkan backend is not part of this build");
#endif
    return opened;
}

OpenedArm open_vulkan() {
    OpenedArm opened;
    opened.arm = Arm::Vulkan;
    opened.name = "vulkan";
#if defined(TESSERA_TEST_HAS_VULKAN)
    if (process_selects_lavapipe()) {
        return unavailable(Arm::Vulkan,
                           "this process's Vulkan device is the lavapipe reference, not a second "
                           "device to compare against");
    }
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::Vulkan;
    desc.validation = false;
    try {
        opened.device = vulkan::create_vulkan_device(desc);
    } catch (const std::exception&) {
        opened.device.reset();
        return unavailable(Arm::Vulkan, "no Vulkan device could be created");
    }
    if (opened.device == nullptr || opened.device->backend() != rhi::Backend::Vulkan) {
        opened.device.reset();
        return unavailable(Arm::Vulkan, "the Vulkan backend did not return a Vulkan device");
    }
#else
    return unavailable(Arm::Vulkan, "the Vulkan backend is not part of this build");
#endif
    return opened;
}

OpenedArm open_webgpu() {
#if defined(TESSERA_TEST_HAS_WEBGPU)
    return unavailable(Arm::WebGpu,
                       "the WebGPU backend needs a host-provided WGPUDevice; this headless harness "
                       "has none");
#else
    return unavailable(Arm::WebGpu, "the WebGPU backend is not part of this build");
#endif
}

} // namespace

std::string_view arm_name(Arm arm) noexcept {
    switch (arm) {
    case Arm::Null:
        return "null";
    case Arm::Lavapipe:
        return "lavapipe";
    case Arm::Vulkan:
        return "vulkan";
    case Arm::WebGpu:
        return "webgpu";
    case Arm::Metal:
        return "metal";
    }
    return "unknown";
}

std::vector<Arm> parity_arms() {
    return {Arm::Lavapipe, Arm::Vulkan, Arm::WebGpu, Arm::Metal, Arm::Null};
}

OpenedArm open_arm(Arm arm) {
    switch (arm) {
    case Arm::Null:
        return open_null();
    case Arm::Lavapipe:
        return open_lavapipe();
    case Arm::Vulkan:
        return open_vulkan();
    case Arm::WebGpu:
        return open_webgpu();
    case Arm::Metal:
        return unavailable(Arm::Metal, "the Metal backend is not implemented yet (ADR-053)");
    }
    return unavailable(arm, "the requested backend arm is not known");
}

bool selects_only_lavapipe(std::string_view icd_filenames) noexcept {
    if (icd_filenames.empty()) {
        return false;
    }
#if defined(_WIN32)
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    std::size_t begin = 0;
    bool any = false;
    while (begin <= icd_filenames.size()) {
        const std::size_t end = icd_filenames.find(kSeparator, begin);
        const std::string_view entry = icd_filenames.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        if (!entry.empty()) {
            any = true;
            if (!is_lavapipe_manifest(entry)) {
                return false;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return any;
}

bool process_selects_lavapipe() noexcept {
    std::string icd = env_value("VK_ICD_FILENAMES");
    if (icd.empty()) {
        icd = env_value("VK_DRIVER_FILES");
    }
    return selects_only_lavapipe(icd);
}

rhi::PipelineDesc pipeline_for(const OpenedArm& arm) {
#if defined(TESSERA_TEST_HAS_VULKAN)
    if ((arm.arm == Arm::Vulkan || arm.arm == Arm::Lavapipe) && arm.device != nullptr &&
        arm.device->backend() == rhi::Backend::Vulkan) {
        return vulkan::batched2d_pipeline_desc();
    }
#endif
    static_cast<void>(arm);
    return rhi::PipelineDesc{};
}

} // namespace tessera::tests::gpu::backends
