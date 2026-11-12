// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/artifact_sink.hpp"
#include "tessera/harness/image_compare.hpp"
#include "tessera/harness/json_value.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace tessera::harness {

// What `--mode golden` / `--mode gpu-smoke` ask of the CLI after the script ran: render the
// scene's final state offscreen and compare it with one baseline PNG.
struct CliFrameRequest {
    CompareMode mode = CompareMode::Golden;
    std::string baseline_path;
    // `--update-goldens` (ADR-009). Only honoured for Golden; the CLI rejects it for GpuSmoke.
    bool update_baseline = false;
    // Empty uses the repository test font (deterministic goldens never use system fonts).
    std::string font_path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float dpi_scale = 1.0f;
};

// Renders `root` with `stylesheet`, checks it against the baseline, writes frame.png / diff /
// compare.json under `output_directory`, and adds a `frame` object to `report`. Returns the
// documented exit code: 0 match (or baseline rewritten), 1 mismatch or missing baseline, 2 the
// machine cannot run the mode or an artifact could not be written. The scene is rendered as the
// script left it; transient input state (hover, focus rings) is not replayed.
[[nodiscard]] int run_cli_frame_check(const CliFrameRequest& request, ui::Node& root,
                                      std::string_view stylesheet,
                                      std::string_view output_directory, ArtifactSink& sink,
                                      JsonValue& report, std::string_view command,
                                      std::ostream& err);

} // namespace tessera::harness
