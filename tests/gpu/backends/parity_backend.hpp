// SPDX-License-Identifier: MIT
//
// Backend arms for the cross-backend image parity harness (issue #245). An arm is one RHI device
// the scene set is rendered on: the lavapipe software Vulkan reference (ADR-009), the host's own
// Vulkan device, WebGPU, Metal (ADR-053, not implemented yet) and the null backend (structure
// only, because it has no image output). Opening an arm never throws: a build without a backend or
// a host without a device yields an unavailable arm carrying a fixed, machine-independent reason,
// so the harness names the skip instead of passing silently.

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::tests::gpu::backends {

enum class Arm : std::uint8_t {
    Null,
    Lavapipe,
    Vulkan,
    WebGpu,
    Metal,
};

[[nodiscard]] std::string_view arm_name(Arm arm) noexcept;

// Every known arm, reference first: {Lavapipe, Vulkan, WebGpu, Metal, Null}.
[[nodiscard]] std::vector<Arm> parity_arms();

struct OpenedArm {
    Arm arm = Arm::Null;
    std::string name;
    std::unique_ptr<rendering::rhi::Device> device;
    // True for a backend that records commands but produces no pixels (null). Its arm is reported
    // and its command counters are checked, but it never takes part in a pixel comparison.
    bool structure_only = false;
    // Fixed text explaining why the arm has no device. Empty when `device` is set.
    std::string unavailable_reason;

    [[nodiscard]] bool available() const noexcept {
        return device != nullptr;
    }
};

[[nodiscard]] OpenedArm open_arm(Arm arm);

// The harness's software-reference rule, same as the golden frame-capture capability check: a
// process whose Vulkan ICD list names only lavapipe is running the software reference.
[[nodiscard]] bool selects_only_lavapipe(std::string_view icd_filenames) noexcept;
[[nodiscard]] bool process_selects_lavapipe() noexcept;

// The pipeline the arm's device can draw the scene set with. Vulkan consumes SPIR-V, so it reuses
// the batched2d pipeline fixture; the null backend records the default descriptor.
[[nodiscard]] rendering::rhi::PipelineDesc pipeline_for(const OpenedArm& arm);

} // namespace tessera::tests::gpu::backends
