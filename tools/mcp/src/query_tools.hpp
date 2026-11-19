// SPDX-License-Identifier: MIT

#pragma once

#include "preview_tools.hpp"
#include "tessera/harness/json_value.hpp"
#include "tool_registry.hpp"

#include <cstdint>
#include <string_view>

// The read-only inspection tools of the MCP surface (docs/specs/designer-studio-v1.md section 8):
// `query_style` and `pick`.
//
// **Queries, not events.** Both lay the source out at the session's viewport exactly as `snapshot`
// does (one `prepare_scene()`), then read the tree: `query_style` through `style_info_for()`, the
// function behind the studio's `style.info`, and `pick` through `Node::hit_test_deep`, the router's
// own hit test. Neither dispatches an input event, moves a pseudo-class or changes anything, so an
// agent inspecting a node cannot alter the picture it is inspecting.
//
// The documents carry the field names and versions of the studio's reports (`styleVersion`,
// `pickVersion`), so an agent and the studio describe a node the same way; the encoding is this
// server's canonical JSON, not the canvas module's one-line writer.
namespace tessera::mcp {

// `query_style`: the computed style, the matched rules (highest precedence first, with the
// cascade's own `overridden`) and the layout box of the node with `ui_id`, as
// `{"styleVersion":1,"ok":true,"node":"<id>","computed":{...},"matched":[...],"box":{...}}`.
// A node that does not exist is `{"styleVersion":1,"ok":false,"reason":"no_such_node"}` and a
// successful result, as in the studio: the question was well formed and the answer is "no such
// node". A source that does not evaluate is an `isError` result naming the stage that failed.
[[nodiscard]] ToolResult run_query_style_tool(const ViewportState& viewport,
                                              std::string_view source, std::string_view file,
                                              std::uint64_t ui_id);

// `pick`: the node under `(x, y)` in logical viewport pixels - the space the `snapshot`'s bounds
// are in - as `{"pickVersion":1,"found":true,"automationId":"<id>","elementKind":"Button",
// "box":{...}}`. A point over no node is `found:false`, a real answer and not an error.
[[nodiscard]] ToolResult run_pick_tool(const ViewportState& viewport, std::string_view source,
                                       std::string_view file, float x, float y);

} // namespace tessera::mcp
