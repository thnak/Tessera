// SPDX-License-Identifier: MIT
//
// The canvas module's device-free helpers (issue #390): the clear-frame and the page's pointer
// vocabulary. Split out of `wasm_canvas_entry.cpp`, which is the WebGPU entry point and at the
// 500-line hard limit, and which is the only place in this directory that includes a backend
// header.
//
// Both functions are pure over their arguments -- no module state, no device creation, no clock --
// so they can be (and are) exercised by the host lanes, which cannot compile the entry point at
// all.

#include "wasm_canvas_support.hpp"

#include <memory>

namespace tessera::compiler::codegen::cli {
namespace {

namespace rhi = tessera::rendering::rhi;

} // namespace

void clear_canvas(rhi::Device& device, const rhi::RenderTarget& target, rhi::Extent2D extent,
                  rhi::TextureFormat format) {
    try {
        std::unique_ptr<rhi::CommandList> commands = device.create_command_list();
        if (commands == nullptr) {
            return;
        }
        rhi::RenderTargetDesc desc;
        desc.extent = extent;
        desc.format = format;
        desc.clear_color = scene_frame_clear_color();
        desc.load_op = rhi::LoadOp::Clear;
        desc.store_op = rhi::StoreOp::Store;
        commands->begin_render_pass(target, desc);
        commands->end_render_pass();
        commands->submit_async();
    } catch (...) {
    }
}

PreviewInput::Kind pointer_kind_from(std::string_view name, bool& known) {
    known = true;
    if (name == "move") {
        return PreviewInput::Kind::PointerMove;
    }
    if (name == "down") {
        return PreviewInput::Kind::PointerDown;
    }
    if (name == "up") {
        return PreviewInput::Kind::PointerUp;
    }
    if (name == "cancel") {
        return PreviewInput::Kind::PointerCancel;
    }
    known = false;
    return PreviewInput::Kind::PointerMove;
}

} // namespace tessera::compiler::codegen::cli
