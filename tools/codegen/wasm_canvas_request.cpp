// SPDX-License-Identifier: MIT

#include "wasm_canvas_font.hpp"
#include "wasm_canvas_request.hpp"

#include <optional>
#include <string>

namespace tessera::compiler::codegen::cli {

namespace {

// The page's salt: the project namespace it shows. The same constant the scene module uses, so the
// two views name one node by one number (`tessera_scene_json` and `tessera_canvas_*` agree).
constexpr std::string_view kPreviewSalt = "preview";

} // namespace

std::string posture_from(std::string_view text, hal::FoldPosture& out) {
    out = hal::FoldPosture::None;
    if (text.empty()) {
        return {};
    }
    const std::optional<hal::FoldPosture> parsed = hal::parse_fold_posture(text);
    if (!parsed) {
        return "TESSERA_STUDIO_FOLD_POSTURE: `" + std::string(text) +
               "` is not a fold posture; expected flat, book or tabletop";
    }
    out = *parsed;
    return {};
}

std::string canvas_session_source(std::string_view source, std::string_view file,
                                  std::string_view component, std::string_view posture_text,
                                  std::string_view stylesheet, float width, float height, float dpi,
                                  CanvasSessionSource& out) {
    hal::FoldPosture posture = hal::FoldPosture::None;
    const std::string posture_error = posture_from(posture_text, posture);
    if (!posture_error.empty()) {
        return posture_error;
    }
    out.text = source;
    out.file = file;
    out.component = component;
    // The author stylesheet (issue #390). Null/empty is "no author CSS": the built-in fallback
    // stays, which is what an older page and a cleared textarea both mean.
    out.stylesheet = stylesheet;
    out.salt = kPreviewSalt;
    // A zero viewport is replaced by the module's long-standing 320x240 default rather than
    // refused: a page that has not laid the canvas out yet still gets a preview, and the counters
    // it reports are the ones a default-sized device would produce.
    out.width = width > 0.0f ? width : 320.0F;
    out.height = height > 0.0f ? height : 240.0F;
    out.dpi_scale = dpi > 0.0f ? dpi : 1.0F;
    out.posture = posture;
    // The session measures its text with the same face every frame paints with, so a line never
    // overflows or under-fills the box it was laid out in (#387).
    out.measurer = canvas_measurer();
    return {};
}

CanvasViewportRequest canvas_viewport_request(int width, int height, float dpi) {
    CanvasViewportRequest request;
    request.width = width > 0 ? static_cast<float>(width) : 0.0f;
    request.height = height > 0 ? static_cast<float>(height) : 0.0f;
    request.dpi = dpi > 0.0f ? dpi : 1.0f;
    if (request.width <= 0.0f || request.height <= 0.0f) {
        // Refused, not defaulted: a toolbar that sent 0 would otherwise lay the tree out at the
        // module's 320x240 default while the page believed it had asked for something else.
        request.error = "the viewport must be positive in both dimensions";
    }
    return request;
}

} // namespace tessera::compiler::codegen::cli
