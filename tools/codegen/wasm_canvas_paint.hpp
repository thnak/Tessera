// SPDX-License-Identifier: MIT

#pragma once

#include "scene_canvas_session.hpp"
#include "scene_frame.hpp"
#include "scene_preview.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <string_view>

// Painting a frame into the canvas texture the page holds (issues #167, #388, #444).
//
// **Why this is not in `wasm_canvas_entry.cpp`.** The entry is the C ABI: it owns the module's one
// device and one session, validates the arguments and returns report strings. This file is the step
// between "there is a tree" and "there are pixels in the page's canvas", and it needs the WebGPU
// target-creation call that the entry should not be reading about. Splitting it kept the entry
// inside the repository's 500-line limit when the live edit added `tessera_canvas_apply`, and the
// boundary is real rather than cosmetic: every function here takes the device and the evaluation it
// paints, so none of them can reach the module's globals.
//
// It is Emscripten-only (it includes the WebGPU backend header), which is why it lives in the
// `tessera_canvas_wasm` target and not in the host-buildable serializer list.
namespace tessera::compiler::codegen::cli {

// The canvas format the page configures. RGBA8 is one of the two formats WebGPU requires every
// canvas to support, and it is the format the paint path and the lavapipe goldens already use, so
// the preview paints the same bytes the reference does.
inline constexpr rendering::rhi::TextureFormat kCanvasFormat =
    rendering::rhi::TextureFormat::R8G8B8A8Unorm;

// Fills the canvas with the preview's clear colour and nothing else.
//
// A canvas texture is presented every frame whether or not anything drew into it, and an undrawn
// one shows as black, so a failed frame would otherwise replace the last picture with a black box
// that looks like a crash. Failure here is swallowed: the report already carries the frame's real
// error.
//
// **It is declared in `wasm_canvas_support.hpp`, not here** (issue #390 split it out of the entry
// file): it needs no device creation and no backend header, so it is host-buildable and this
// Emscripten-only unit is its caller. `wasm_canvas_paint.cpp` includes that header for it.

// Evaluates the source and draws one frame into the canvas texture the page holds.
//
// This is the **stateless** path (`tessera_canvas_frame`): it re-evaluates on every call and
// carries no state between calls. The live edit does not use it -- see `paint_session` below.
SceneFrameResult paint_to_canvas(std::string_view source, const ScenePreviewOptions& options,
                                 std::string_view component, rendering::rhi::Extent2D extent,
                                 void* canvas_view, rendering::rhi::Device& device);

// Paints the session's current tree into the canvas texture the page holds.
//
// Unlike `paint_to_canvas` this never re-evaluates: the tree is the one the pointer events have
// been dispatched into, so its hover/active bits and its state are the ones the frame shows. A null
// `canvas_view` draws into an offscreen target instead, which is how the Node probe reads the
// counters with no surface.
SceneFrameResult paint_session(CanvasSceneSession& session, rendering::rhi::Extent2D extent,
                               void* canvas_view, rendering::rhi::Device& device);

} // namespace tessera::compiler::codegen::cli
