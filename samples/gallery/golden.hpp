// SPDX-License-Identifier: MIT

// Golden comparison of a gallery capture (issue #239, ADR-009): a capture is compared with the
// committed lavapipe baseline under the same 1 LSB + 0.1% budget as the other text goldens.

#pragma once

#include "tessera/app/window.hpp"

#include <string>

namespace gallery_golden {

// False when this machine cannot produce golden frames (the loader does not select only lavapipe);
// `reason` then says why and the caller reports a skip.
[[nodiscard]] bool available(std::string& reason);

// Compares `frame` with `baseline_path`. `artifact_dir` receives frame.png, the diff image and the
// compare report on every run, so CI can upload them for a missing or changed baseline. With
// `update` the baseline is rewritten from the frame instead of failing. Returns a process exit code.
[[nodiscard]] int check(const tessera::app::CapturedFrame& frame, const std::string& baseline_path,
                        const std::string& artifact_dir, bool update);

} // namespace gallery_golden
