// SPDX-License-Identifier: MIT

#pragma once

#include "scene_frame.hpp"
#include "scene_session.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <string>
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
// frame would otherwise replace the last picture with a black box that looks like a crash.
//
// Returns an empty string on success and a description of the failure otherwise. It does NOT throw
// and does not swallow (issue #733 item 5): the caller is already reporting the frame's real error,
// so it cannot *replace* that error, but a silent `catch (...)` hid a failed clear from every
// reader. The caller appends this to its report instead of losing it.
[[nodiscard]] std::string clear_canvas(tessera::rendering::rhi::Device& device,
                                       const tessera::rendering::rhi::RenderTarget& target,
                                       tessera::rendering::rhi::Extent2D extent,
                                       tessera::rendering::rhi::TextureFormat format);

// The normalized pointer kind `name` spells, or `false` in `known` when it spells none. An
// unrecognized kind is reported rather than coerced to a move: a typo in the page would otherwise
// move the hover somewhere the user never pointed.
[[nodiscard]] PreviewInput::Kind pointer_kind_from(std::string_view name, bool& known);

// The page's keyboard vocabulary (issue #388, focus): `"down"` and `"up"` map to a key event, and
// `known` is false for anything else (`"blur"` is handled by the caller, which has no key to map).
[[nodiscard]] PreviewInput::Kind key_kind_from(std::string_view name, bool& known);

// The page's text vocabulary (issue #601): `"commit"` is committed text, `"start"`, `"update"` and
// `"end"` are the IME composition lifecycle. `known` is false for anything else (`"copy"` and
// `"cut"` are handled by the caller, which has no text to send).
[[nodiscard]] PreviewInput::Kind text_kind_from(std::string_view name, bool& known);

// The engine's logical key code for a W3C `KeyboardEvent.key` value ("Tab", "Enter", " ",
// "ArrowDown", "a"): the same names `key_is` in `event_payloads.hpp` compares against. A single
// printable character maps to its upper-case ASCII code; an unknown name maps to 0, which the page
// treats as "not a key this canvas takes".
[[nodiscard]] std::uint32_t key_code_from_name(std::string_view name);

} // namespace tessera::compiler::codegen::cli
