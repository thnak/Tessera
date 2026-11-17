// SPDX-License-Identifier: MIT

#pragma once

#include "scene_frame.hpp"
#include "scene_session.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <string_view>

// The two pieces of the canvas module's ABI that are not the entry points themselves (issue #390).
//
// They live here rather than in `wasm_canvas_entry.cpp` for one reason that matters to this
// project: that file is the WebGPU entry point, it is at the 500-line hard limit, and the two
// functions below are independent of the device handover and of the session. `clear_canvas` is a
// self-contained frame, and `pointer_kind_from` is the page's pointer vocabulary -- neither needs
// the module's globals.
namespace tessera::compiler::codegen::cli {

// Fills `target` with the preview's clear colour and nothing else. A canvas texture is presented
// every frame whether or not anything drew into it, and an undrawn one shows as black, so a failed
// frame would otherwise replace the last picture with a black box that looks like a crash. Failure
// is swallowed: the caller's report already carries the frame's real error.
void clear_canvas(tessera::rendering::rhi::Device& device,
                  const tessera::rendering::rhi::RenderTarget& target,
                  tessera::rendering::rhi::Extent2D extent,
                  tessera::rendering::rhi::TextureFormat format);

// The normalized pointer kind `name` spells, or `false` in `known` when it spells none. An
// unrecognized kind is reported rather than coerced to a move: a typo in the page would otherwise
// move the hover somewhere the user never pointed.
[[nodiscard]] PreviewInput::Kind pointer_kind_from(std::string_view name, bool& known);

} // namespace tessera::compiler::codegen::cli
