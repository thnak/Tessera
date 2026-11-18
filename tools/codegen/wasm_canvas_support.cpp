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

#include "tessera/ui/key_code.hpp"
#include "wasm_canvas_support.hpp"

#include <cstdint>
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

PreviewInput::Kind key_kind_from(std::string_view name, bool& known) {
    known = true;
    if (name == "down") {
        return PreviewInput::Kind::KeyDown;
    }
    if (name == "up") {
        return PreviewInput::Kind::KeyUp;
    }
    known = false;
    return PreviewInput::Kind::KeyDown;
}

std::uint32_t key_code_from_name(std::string_view name) {
    struct Named {
        std::string_view name;
        std::uint32_t code;
    };
    constexpr Named kNamed[] = {
        {"Backspace", ui::key::kBackspace},
        {"Tab", ui::key::kTab},
        {"Enter", ui::key::kEnter},
        {"Escape", ui::key::kEscape},
        {" ", ui::key::kSpace},
        {"Insert", ui::key::kInsert},
        {"Delete", ui::key::kDelete},
        {"Home", ui::key::kHome},
        {"End", ui::key::kEnd},
        {"PageUp", ui::key::kPageUp},
        {"PageDown", ui::key::kPageDown},
        {"ArrowLeft", ui::key::kArrowLeft},
        {"ArrowUp", ui::key::kArrowUp},
        {"ArrowRight", ui::key::kArrowRight},
        {"ArrowDown", ui::key::kArrowDown},
    };
    for (const Named& entry : kNamed) {
        if (entry.name == name) {
            return entry.code;
        }
    }
    if (name.size() == 1) {
        const char c = name[0];
        return (c >= 'a' && c <= 'z') ? static_cast<std::uint32_t>(c - 'a' + 'A')
                                      : static_cast<std::uint32_t>(static_cast<unsigned char>(c));
    }
    return 0;
}

} // namespace tessera::compiler::codegen::cli
