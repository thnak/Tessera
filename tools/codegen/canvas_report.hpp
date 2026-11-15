// SPDX-License-Identifier: MIT

#pragma once

#include "scene_frame.hpp"

#include <cstdint>
#include <string>

// The report the canvas module returns to the page (issue #167, P3 of
// docs/design/reference-equantic-ui.md section 7.4).
//
// It is a plain C++ function in its own translation unit, not a lambda inside the Emscripten entry,
// so that the host lanes compile it too: `wasm_canvas_entry.cpp` includes the WebGPU backend header
// and can only be built by Emscripten, and the serializer is the part of that entry which can still
// be type-checked (and tested) on a normal Windows/Linux build. Same shape the P1/P2 modules use,
// adapted to an entry the host cannot include.
namespace tessera::compiler::codegen::cli {

// One JSON object, one line, with the field names the native CLI prints. `has_device` distinguishes
// "the page never handed over a WebGPU device" from "the frame itself failed", which are different
// things for a page to report: the first is an unsupported browser, the second is a bug.
[[nodiscard]] std::string canvas_report_json(const SceneFrameResult& result, std::uint32_t width,
                                             std::uint32_t height, bool has_device);

} // namespace tessera::compiler::codegen::cli
