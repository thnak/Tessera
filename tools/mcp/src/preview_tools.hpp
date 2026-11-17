// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/fold_posture.hpp"
#include "tool_registry.hpp"

#include <string>
#include <string_view>

// The preview tools of the MCP surface (docs/specs/designer-studio-v1.md section 8): `set_viewport`
// and `snapshot`.
//
// **One engine, two front ends.** These tools are the agent-facing half of the studio's preview
// (issue #408). They do not lay anything out themselves: they fill in a
// `cli::ScenePreviewOptions` and call `scene_preview_document()`, which is the function the browser
// module's `tessera_scene_json_folded` export also calls. So a posture set through MCP reaches the
// same `hal::fold_info_for()` -> `WindowMetrics::fold` -> `hal::resolve_safe_area()` path a
// posture chosen in the studio does, and the two produce the same document for the same input.
namespace tessera::mcp {

// The viewport a session previews at. It is the tool's state: `snapshot` renders whatever
// `set_viewport` last set, the way `viewport.set` in the designer protocol is state rather than a
// one-shot argument.
//
// Defaults are the preview's own (320x240 at DPR 1, no fold), so a `snapshot` with no
// `set_viewport` before it answers the same document `tessera_scene_preview` writes for its
// defaults - which is what the parity gate compares.
struct ViewportState {
    float width = 320.0f;
    float height = 240.0f;
    float dpr = 1.0f;
    tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None;
};

// The result of `set_viewport`: the viewport the session now holds, echoed back so a client can
// confirm what a later `snapshot` will render at without a second call.
[[nodiscard]] ToolResult run_set_viewport_tool(ViewportState& viewport, std::string_view width,
                                               std::string_view height, std::string_view dpr,
                                               std::string_view fold_posture);

// `snapshot`: the canonical UiSnapshot document (docs/specs/automation-ids-v1.md section 5.2) for
// `source` at the session's viewport. Reads no file and touches no clock, so two runs agree byte
// for byte - the property the studio's parity gate relies on.
[[nodiscard]] ToolResult run_snapshot_tool(const ViewportState& viewport, std::string_view source,
                                           std::string_view file);

} // namespace tessera::mcp
