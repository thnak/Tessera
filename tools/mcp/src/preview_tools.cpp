// SPDX-License-Identifier: MIT

#include "preview_tools.hpp"
#include "scene_preview.hpp"
#include "tessera/hal/fold_posture.hpp"

#include <charconv>
#include <cmath>
#include <optional>
#include <system_error>

namespace tessera::mcp {
namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace hal = tessera::hal;

// One positive finite number from a text argument, or 0 when it is anything else. The caller
// reports the rejection; a clamped guess would make a typo look like a small viewport.
float parse_positive(std::string_view text) {
    if (text.empty() || text.size() > 32) {
        return 0.0f;
    }
    float value = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) ||
        value <= 0.0f) {
        return 0.0f;
    }
    return value;
}

std::string number_text(float value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, result.ptr);
}

// The `set_viewport` success payload. It is JSON so a client can read the fields rather than a
// sentence, and it echoes the whole viewport (not just the fields that changed), because the next
// `snapshot` renders at all of them.
std::string viewport_json(const ViewportState& viewport) {
    std::string out = "{\"width\":";
    out += number_text(viewport.width);
    out += ",\"height\":";
    out += number_text(viewport.height);
    out += ",\"dpr\":";
    out += number_text(viewport.dpr);
    out += ",\"foldPosture\":\"";
    out += hal::fold_posture_name(viewport.posture);
    out += "\"}";
    return out;
}

} // namespace

ToolResult run_set_viewport_tool(ViewportState& viewport, std::string_view width,
                                 std::string_view height, std::string_view dpr,
                                 std::string_view fold_posture) {
    // Omitted fields keep their value, which is `viewport.set`'s rule in
    // docs/specs/designer-protocol-v1.md section 6: a client changing only the posture must not
    // have to resend the size.
    float next_width = viewport.width;
    float next_height = viewport.height;
    float next_dpr = viewport.dpr;
    hal::FoldPosture next_posture = viewport.posture;

    if (!width.empty()) {
        next_width = parse_positive(width);
        if (next_width == 0.0f) {
            return ToolResult{true, "set_viewport: `width` must be a positive number"};
        }
    }
    if (!height.empty()) {
        next_height = parse_positive(height);
        if (next_height == 0.0f) {
            return ToolResult{true, "set_viewport: `height` must be a positive number"};
        }
    }
    if (!dpr.empty()) {
        next_dpr = parse_positive(dpr);
        if (next_dpr == 0.0f) {
            return ToolResult{true, "set_viewport: `dpr` must be a positive number"};
        }
    }
    if (!fold_posture.empty()) {
        // The posture set is the protocol's (`flat`, `book`, `tabletop`), resolved by the HAL's one
        // parser. An unknown name is refused rather than treated as `flat`: a client whose typo
        // silently previewed an unfolded screen would get a snapshot that looks right and is not.
        const std::optional<hal::FoldPosture> parsed = hal::parse_fold_posture(fold_posture);
        if (!parsed) {
            return ToolResult{true, "set_viewport: `foldPosture` must be flat, book or tabletop"};
        }
        next_posture = *parsed;
    }

    viewport.width = next_width;
    viewport.height = next_height;
    viewport.dpr = next_dpr;
    viewport.posture = next_posture;
    return ToolResult{false, viewport_json(viewport)};
}

ToolResult run_snapshot_tool(const ViewportState& viewport, std::string_view source,
                             std::string_view file) {
    // The SAME driver the browser module and the native preview CLI call, with the viewport the
    // session holds. This is what makes "the same posture set through MCP gives the same scene
    // snapshot" a property of the code rather than a coincidence: there is one layout path.
    cli::ScenePreviewOptions options;
    options.file = file.empty() ? std::string_view("ui/Preview.tsx") : file;
    options.width = viewport.width;
    options.height = viewport.height;
    options.dpi_scale = viewport.dpr;
    options.posture = viewport.posture;
    // A salted run, matching what the studio passes, so a `snapshot`'s `uiId`s are the ones the
    // native build of the same source assigns (docs/specs/automation-ids-v1.md section 2.1).
    options.salt = "preview";
    return ToolResult{false, cli::scene_preview_document(source, options, {})};
}

} // namespace tessera::mcp
