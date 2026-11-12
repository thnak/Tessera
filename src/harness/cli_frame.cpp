// SPDX-License-Identifier: MIT

#include "tessera/harness/cli_frame.hpp"
#include "tessera/harness/frame_check.hpp"
#ifdef TESSERA_HARNESS_HAS_CAPTURE
#include "tessera/harness/frame_capture.hpp"
#endif

#include <string>
#include <utility>

namespace tessera::harness {

#ifdef TESSERA_HARNESS_HAS_CAPTURE

int run_cli_frame_check(const CliFrameRequest& request, ui::Node& root, std::string_view stylesheet,
                        std::string_view output_directory, ArtifactSink& sink, JsonValue& report,
                        std::string_view command, std::ostream& err) {
    app::CaptureOptions options;
    options.width = request.width;
    options.height = request.height;
    options.dpi_scale = request.dpi_scale;
#ifdef TESSERA_HARNESS_DEFAULT_FONT
    options.font_path =
        request.font_path.empty() ? TESSERA_HARNESS_DEFAULT_FONT : request.font_path;
#else
    options.font_path = request.font_path;
#endif
    const auto frame = capture_scene_frame(request.mode, root, stylesheet, options);
    if (!frame.has_value()) {
        err << "tessera " << command << ": " << to_string(request.mode)
            << " frame unavailable: " << frame.error().message << "\n";
        return 2;
    }

    FrameCheckRequest check;
    check.mode = request.mode;
    check.baseline_path = request.baseline_path;
    check.artifact_prefix = std::string(output_directory) + "/";
    check.update_baseline = request.update_baseline;
    const auto result = check_frame(*frame, check, sink);
    if (!result.has_value()) {
        err << "tessera " << command << ": frame check failed: " << result.error().message << "\n";
        return 2;
    }

    JsonValue summary = JsonValue::object();
    summary.set("mode", JsonValue::string(std::string(to_string(request.mode))));
    summary.set("baseline", JsonValue::string(request.baseline_path));
    summary.set("baselineUpdated", JsonValue::boolean(result->baseline_updated));
    summary.set("passed", JsonValue::boolean(result->ok()));
    summary.set("frame", JsonValue::string(result->frame_artifact));
    summary.set("report", JsonValue::string(result->report_artifact));
    report.set("frame", std::move(summary));

    if (!result->ok()) {
        err << "tessera " << command << ": " << to_string(request.mode)
            << " frame does not match baseline '" << request.baseline_path << "' (see "
            << result->report_artifact << ")\n";
        return 1;
    }
    return 0;
}

#else

int run_cli_frame_check(const CliFrameRequest& request, ui::Node&, std::string_view,
                        std::string_view, ArtifactSink&, JsonValue&, std::string_view command,
                        std::ostream& err) {
    err << "tessera " << command << ": --mode " << to_string(request.mode)
        << " needs the Vulkan rendering lane, which this build does not include\n";
    return 2;
}

#endif

} // namespace tessera::harness
