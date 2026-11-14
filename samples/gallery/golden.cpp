// SPDX-License-Identifier: MIT

#include "golden.hpp"
#include "tessera/harness/artifact_sink.hpp"
#include "tessera/harness/frame_capture.hpp"
#include "tessera/harness/frame_check.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace gallery_golden {

bool available(std::string& reason) {
    const auto capability =
        tessera::harness::capture_capability(tessera::harness::CompareMode::Golden);
    if (capability) {
        return true;
    }
    reason = capability.error().message;
    return false;
}

int check(const tessera::app::CapturedFrame& frame, const std::string& baseline_path,
          const std::string& artifact_dir, bool update) {
    namespace harness = tessera::harness;
    harness::RgbaImage image;
    image.width = frame.width;
    image.height = frame.height;
    image.rgba = frame.rgba;

    harness::FrameCheckRequest request;
    request.mode = harness::CompareMode::Golden;
    // Glyph antialiasing rounds differently across lavapipe/LLVM revisions: the budget the
    // batched2d text goldens use (docs/specs/golden-tests-v1.md section 1), not bit-exact.
    request.policy = harness::ComparePolicy{1, 0.001};
    request.baseline_path = baseline_path;
    request.update_baseline = update;
    request.artifact_prefix = artifact_dir.empty() ? std::string() : artifact_dir + "/";

    harness::FileArtifactSink sink;
    const auto checked = harness::check_frame(image, request, sink);
    if (!checked) {
        std::cerr << "gallery: golden check failed: " << checked.error().message << "\n";
        return 1;
    }
    if (checked->ok()) {
        std::cout << "gallery: golden " << (checked->baseline_updated ? "updated " : "matches ")
                  << baseline_path << "\n";
        return 0;
    }
    std::cerr << "gallery: golden " << harness::to_string(checked->compare.outcome) << " for "
              << baseline_path << " (artifacts in " << artifact_dir << ")\n";
    return 1;
}

} // namespace gallery_golden
