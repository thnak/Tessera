// SPDX-License-Identifier: MIT

#include "wasm_canvas_font.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

// The preview's one face (#387), loaded from bytes the page fetched. Empty until
// `tessera_canvas_set_font` succeeds, and every frame in that state paints boxes only and reports
// `textSkipped` - the documented degraded preview rather than a broken one.
std::unique_ptr<PreviewTextStack> g_text;
// Why the last `canvas_set_font` refused its bytes, for the status report.
std::string g_font_error;

// Minimal JSON string escaping: the family name is a font's own, so quotes and backslashes are the
// only bytes that can break the object. Control characters cannot appear in a family name that
// FreeType accepted, and the reason strings above are ours.
void append_escaped(std::string& out, std::string_view text) {
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            out.push_back('\\');
        }
        out.push_back(character);
    }
}

} // namespace

PreviewTextStack* canvas_text_stack() noexcept {
    return g_text.get();
}

const paint::PaintTextMetricsSource* canvas_paint_metrics() noexcept {
    return g_text != nullptr ? &g_text->paint_metrics() : nullptr;
}

std::shared_ptr<const text::TextMeasurer> canvas_measurer() {
    return g_text != nullptr ? g_text->measurer() : nullptr;
}

int canvas_set_font(const unsigned char* bytes, int size, rhi::Device* device) {
    // A second call replaces the first face outright, including a fresh atlas: the atlas keys
    // entries by (glyph id, pixel size), so two faces' glyph ids would otherwise share one page.
    g_text.reset();
    g_font_error.clear();
    if (bytes == nullptr || size <= 0) {
        g_font_error = "the page supplied no font bytes";
        return 0;
    }
    if (device == nullptr) {
        // The atlas renderer creates GPU page textures, so a device must exist first. Naming the
        // order is more useful than a silent failure that only shows up as boxes on the first
        // frame.
        g_font_error =
            "no WebGPU device: call tessera_webgpu_set_device before tessera_canvas_set_font";
        return -1;
    }
    const std::span<const unsigned char> data(bytes, static_cast<std::size_t>(size));
    auto loaded =
        PreviewTextStack::create(std::span<const std::uint8_t>(data.data(), data.size()), *device);
    if (!loaded) {
        g_font_error = loaded.error().message;
        return -1;
    }
    g_text = std::move(*loaded);
    return 1;
}

std::string canvas_font_status_json() {
    std::string out = "{\"fontVersion\":1";
    out += g_text != nullptr ? ",\"loaded\":true" : ",\"loaded\":false";
    if (g_text != nullptr) {
        out += ",\"family\":\"";
        append_escaped(out, g_text->face_name());
        out += "\"";
        const PreviewTextCounters counters = g_text->counters();
        out += ",\"glyphsDropped\":" + std::to_string(counters.glyphs_dropped);
        out += ",\"atlasPages\":" + std::to_string(counters.atlas_pages);
        out += ",\"atlasEvictions\":" + std::to_string(counters.atlas_evictions);
    }
    out += ",\"error\":\"";
    append_escaped(out, g_font_error);
    out += "\"}\n";
    return out;
}

} // namespace tessera::compiler::codegen::cli
