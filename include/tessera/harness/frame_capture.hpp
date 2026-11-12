// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/app/window.hpp"
#include "tessera/harness/image_compare.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace tessera::harness {

enum class CaptureErrorCode : std::uint8_t {
    NoVulkanDevice,   // the machine exposes no usable Vulkan device
    NoSoftwareDevice, // golden needs the lavapipe/software path and it is not selected
    RenderFailed,     // the renderer threw while recording or reading back
};

struct CaptureError {
    CaptureErrorCode code = CaptureErrorCode::RenderFailed;
    std::string message;

    // True for the two "this machine cannot run the mode" codes, which callers report as
    // unavailable/skipped rather than as a test failure.
    [[nodiscard]] bool unavailable() const noexcept {
        return code != CaptureErrorCode::RenderFailed;
    }
};

// True when a Vulkan ICD manifest list (the value of VK_ICD_FILENAMES / VK_DRIVER_FILES) selects
// only Mesa lavapipe. The RHI does not report device type, so the software path is established by
// the loader environment: an ICD list that names anything besides lavapipe, or none at all, could
// expose a hardware device and is not accepted for golden frames.
[[nodiscard]] bool selects_only_software_icd(std::string_view icd_filenames) noexcept;

// Golden requires the software path; GpuSmoke accepts whatever device the RHI picks. Reads the
// process environment; never creates a device, so it cannot touch a (possibly crashing) driver.
[[nodiscard]] std::expected<void, CaptureError> capture_capability(CompareMode mode);

// Same rule with the environment values passed in (tests, and drivers that own their environment).
[[nodiscard]] std::expected<void, CaptureError> capture_capability(CompareMode mode,
                                                                   std::string_view icd_filenames);

// Renders one frame of `root` offscreen through the app host + batched 2D renderer (no
// swapchain) and reads it back as RGBA8. `prepare` runs after the first layout frame and before
// the captured frame, exactly as in `app::capture_frame`. Checks `capture_capability(mode)` first,
// so on a machine without the required device this returns a typed error before any Vulkan work.
[[nodiscard]] std::expected<RgbaImage, CaptureError>
capture_scene_frame(CompareMode mode, ui::Node& root, std::string_view stylesheet,
                    const app::CaptureOptions& options, const std::function<void()>& prepare = {});

} // namespace tessera::harness
