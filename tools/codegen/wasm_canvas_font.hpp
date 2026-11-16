// SPDX-License-Identifier: MIT

#pragma once

#include "preview_text.hpp"
#include "tessera/rendering/rhi/device.hpp"

// The canvas module's one font (issue #387), and the two C exports the page uses to supply it.
//
// **Why this is a translation unit of its own.** The entry point (`wasm_canvas_entry.cpp`) is a
// list of C exports over one global session; the font is a second, independent piece of module
// state with its own lifetime, its own failure vocabulary and its own JSON report. Keeping it here
// leaves the entry point at the size the source-size rule allows, and it means the module's text
// state can be read by a test without instantiating a device or a session.
//
// **The bytes come from the page.** This module has no filesystem (no `-sNODERAWFS`, by design:
// docs/design/compiler-wasm.md section 2.1), so `tessera_canvas_set_font` takes a pointer into the
// module's heap that the page filled from a same-origin fetch. The module copies them into a
// `FontFace` and forgets the pointer, which is why the page may free its buffer immediately.
namespace tessera::compiler::codegen::cli {

// The loaded stack, or null before the page has supplied usable bytes. Every frame reads it to
// decide whether to paint text; a null stack is the documented degraded preview (boxes painted,
// runs counted), not an error state.
[[nodiscard]] PreviewTextStack* canvas_text_stack() noexcept;

// The running evaluation's text-measurement seam: the loaded face's measurer, or null when no font
// has been accepted. A caller passes it to `prepare_scene`/`PreviewSession` so the boxes a frame is
// laid out in match the glyphs it paints.
[[nodiscard]] std::shared_ptr<const text::TextMeasurer> canvas_measurer();

// Installs `font_data` as the module's face, replacing any previous one.
//
// Returns 1 on success, 0 for empty data, -1 when the device is missing or the bytes are unusable,
// and records the reason for `tessera_canvas_font_status`. A refusal keeps whatever face was loaded
// before (or none) and never throws: a failed font must degrade the preview, not break the page.
int canvas_set_font(const unsigned char* bytes, int size, rhi::Device* device);

// The current font's status as one JSON object (see `tessera_canvas_font_status`).
[[nodiscard]] std::string canvas_font_status_json();

} // namespace tessera::compiler::codegen::cli
