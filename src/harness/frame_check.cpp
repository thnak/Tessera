// SPDX-License-Identifier: MIT

#include "tessera/harness/frame_check.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

namespace tessera::harness {
namespace {

std::unexpected<FrameCheckError> fail(FrameCheckErrorCode code, std::string message) {
    return std::unexpected(FrameCheckError{code, std::move(message)});
}

// Loads the baseline. nullopt in the value means "absent"; an unreadable or undecodable file is an
// error, never silently treated as missing (that would let a corrupt baseline pass an update).
std::expected<std::optional<RgbaImage>, FrameCheckError> load_baseline(const std::string& path) {
    if (path.empty()) {
        return std::optional<RgbaImage>{};
    }
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec)) {
        return std::optional<RgbaImage>{};
    }
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) {
        return fail(FrameCheckErrorCode::BaselineIo, "cannot open baseline: " + path);
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    const std::vector<std::uint8_t> bytes(raw.begin(), raw.end());
    auto decoded = decode_png(bytes);
    if (!decoded) {
        return fail(FrameCheckErrorCode::BaselineInvalid, "baseline is not a valid PNG: " + path);
    }
    return std::optional<RgbaImage>{std::move(*decoded)};
}

std::expected<void, FrameCheckError> write_baseline(const std::string& path,
                                                    const std::string& png) {
    const auto target = std::filesystem::path(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(png.data(), static_cast<std::streamsize>(png.size()));
    if (!out) {
        return fail(FrameCheckErrorCode::BaselineIo, "cannot write baseline: " + path);
    }
    return {};
}

std::expected<void, FrameCheckError> to_sink(ArtifactSink& sink, const std::string& path,
                                             const std::string& bytes) {
    if (auto written = sink.write(path, bytes); !written) {
        return fail(FrameCheckErrorCode::Artifact, written.error().message);
    }
    return {};
}

} // namespace

std::expected<FrameCheckResult, FrameCheckError>
check_frame(const RgbaImage& frame, const FrameCheckRequest& request, ArtifactSink& sink) {
    if (request.update_baseline &&
        (request.mode != CompareMode::Golden || request.baseline_path.empty())) {
        return fail(FrameCheckErrorCode::UpdateNotAllowed,
                    "baselines are updated only in golden mode with a baseline path");
    }

    auto png = encode_png(frame);
    if (!png) {
        return fail(FrameCheckErrorCode::Image, png.error().message);
    }
    auto baseline = load_baseline(request.baseline_path);
    if (!baseline) {
        return std::unexpected(std::move(baseline.error()));
    }

    FrameCheckResult result;
    result.frame_artifact = request.artifact_prefix + "frame.png";
    result.diff_artifact.clear();
    result.report_artifact = request.artifact_prefix + "frame.compare.json";
    const ComparePolicy policy = request.policy.value_or(default_policy(request.mode));
    result.compare =
        compare_images(request.mode, frame, baseline->has_value() ? &**baseline : nullptr, policy);

    if (auto status = to_sink(sink, result.frame_artifact, *png); !status) {
        return std::unexpected(std::move(status.error()));
    }

    if (result.compare.outcome == CompareOutcome::Mismatch) {
        auto diff = make_diff_image(frame, **baseline, policy.per_channel_tolerance);
        if (!diff) {
            return fail(FrameCheckErrorCode::Image, diff.error().message);
        }
        auto diff_png = encode_png(*diff);
        if (!diff_png) {
            return fail(FrameCheckErrorCode::Image, diff_png.error().message);
        }
        result.diff_artifact = request.artifact_prefix + "frame.diff.png";
        if (auto status = to_sink(sink, result.diff_artifact, *diff_png); !status) {
            return std::unexpected(std::move(status.error()));
        }
    }

    if (request.update_baseline && !result.compare.passed()) {
        if (auto status = write_baseline(request.baseline_path, *png); !status) {
            return std::unexpected(std::move(status.error()));
        }
        result.baseline_updated = true;
    }

    result.report_json = compare_result_json(result.compare, request.baseline_path);
    if (auto status = to_sink(sink, result.report_artifact, result.report_json); !status) {
        return std::unexpected(std::move(status.error()));
    }
    return result;
}

} // namespace tessera::harness
