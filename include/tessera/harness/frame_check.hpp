// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/artifact_sink.hpp"
#include "tessera/harness/image_compare.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace tessera::harness {

// One frame comparison: write `frame.png`, compare against a baseline PNG on disk, and write
// `frame.diff.png` plus `frame.compare.json` through the artifact sink (docs/specs/
// headless-automation-v1.md section 5). Pure CPU: no device is involved, so the rule set is
// testable without Vulkan.
struct FrameCheckRequest {
    CompareMode mode = CompareMode::Golden;
    // Unset uses `default_policy(mode)`.
    std::optional<ComparePolicy> policy;
    // Filesystem path of the committed baseline PNG; empty means "no baseline".
    std::string baseline_path;
    // Sink-relative directory for the artifacts ("" or e.g. "run1/"; must end in '/' if set).
    std::string artifact_prefix;
    // Rewrites `baseline_path` with the captured frame. Only Golden honours it; GpuSmoke with this
    // set is rejected (ADR-009: GPU runs never produce or update baselines). The CLI/env exposure
    // (`--update-goldens`, ADR-009) is a later slice; this is a parameter only.
    bool update_baseline = false;
};

enum class FrameCheckErrorCode : std::uint8_t {
    UpdateNotAllowed, // update_baseline requested outside Golden mode, or without a baseline path
    BaselineInvalid,  // the baseline file exists but is not a decodable PNG
    BaselineIo,       // reading or writing the baseline failed
    Artifact,         // the artifact sink refused a write
    Image,            // PNG encode or diff construction failed
};

struct FrameCheckError {
    FrameCheckErrorCode code = FrameCheckErrorCode::Image;
    std::string message;
};

struct FrameCheckResult {
    CompareResult compare;
    bool baseline_updated = false;
    std::string frame_artifact;  // sink path of frame.png
    std::string diff_artifact;   // sink path of the diff image; empty when none was written
    std::string report_artifact; // sink path of the compare JSON
    std::string report_json;     // same document, for embedding in report.json

    // A frame passes when it matched, or when this run (re)wrote the baseline on request.
    [[nodiscard]] bool ok() const noexcept {
        return compare.passed() || baseline_updated;
    }
};

[[nodiscard]] std::expected<FrameCheckResult, FrameCheckError>
check_frame(const RgbaImage& frame, const FrameCheckRequest& request, ArtifactSink& sink);

} // namespace tessera::harness
