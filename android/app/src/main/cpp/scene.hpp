// SPDX-License-Identifier: MIT
//
// The Android app shell's scene (issue #189).
//
// It is built by hand rather than from a generated component, and that is deliberate: the codegen
// CLI runs on the HOST at build time (`src/compiler/codegen/CMakeLists.txt` gates
// `tessera_compiler_demo` on `NOT CMAKE_CROSSCOMPILING`), so an Android build cannot generate a
// component. A hand-built tree still exercises everything the shell needs to prove: a scene graph,
// a compiled stylesheet, Yoga layout, the Batched2D renderer, and the input path (a Button that
// reacts to a tap and a TextInput that takes typed text).
//
// When the codegen host tool lands (the same ExternalProject pattern the shader manifest checker
// uses), this file is replaced by the generated `demo::Demo` the desktop click-demo runs, and the
// shell then proves the compiler pipeline on the device too.

#pragma once

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <string>

namespace tessera::android_shell {

// The scene root is the window's content node. The ids are outside any generated range so a later
// switch to a generated component does not collide.
inline constexpr tessera::ui::NodeId kRootId = 900;
inline constexpr tessera::ui::NodeId kCardId = 901;
inline constexpr tessera::ui::NodeId kTitleId = 902;
inline constexpr tessera::ui::NodeId kCountId = 903;
inline constexpr tessera::ui::NodeId kButtonId = 904;
inline constexpr tessera::ui::NodeId kInputId = 905;

// Sets a node's painted text. The payload is the one the paint pass reads; it never affects layout,
// so a text change marks Paint|Render only.
inline void set_text(tessera::ui::Node& node, std::string text) {
    tessera::ui::PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = std::move(text);
    node.set_paint_data(std::move(data));
}

// The shell's scene: a card with a title, a counter, a button that increments it, and a text field.
//
// The counter is a plain `int` the button's handler bumps. It is not a React state cell: the shell
// has no generated component, so there is no `useState` to bind. The point is the input path, not
// the state model.
struct ShellScene {
    ShellScene() {
        root.set_element_kind("View");
        root.set_inline_style("width", "100%");
        root.set_inline_style("height", "100%");
        root.set_inline_style("background-color", "#0b1220");

        // `Node` is neither copyable nor movable (its children own it by `unique_ptr` and its
        // address is its identity), so every node is constructed in place and the tree is built by
        // handing ownership to the parent. The ids are the ones the stylesheet and the tests refer
        // to.
        auto card = std::make_unique<tessera::ui::Node>(kCardId);
        card->set_element_kind("View");
        card->set_inline_style("display", "flex");
        card->set_inline_style("flex-direction", "column");
        card->set_inline_style("padding", "24px");

        auto title = std::make_unique<tessera::ui::Node>(kTitleId);
        title->set_element_kind("Text");
        set_text(*title, "Tessera on Android");

        auto count = std::make_unique<tessera::ui::Node>(kCountId);
        count->set_element_kind("Text");
        set_text(*count, "Taps: 0");
        // The label is borrowed before ownership moves into the tree: the tree owns it, and the tap
        // handler needs a way back to it without a search API the scene graph does not have.
        count_label = count.get();

        auto button = std::make_unique<tessera::ui::Node>(kButtonId);
        button->set_element_kind("Button");
        set_text(*button, "Tap me");
        // The binding runs on the UI thread from the input router, so it may touch the scene. It is
        // a plain function pointer plus a context, which is the allocation-free binding the scene
        // graph offers (docs/design/scene-graph.md section 6).
        button->bind_event(tessera::ui::EventType::PointerDown, &ShellScene::on_button_down, this);

        auto input = std::make_unique<tessera::ui::Node>(kInputId);
        input->set_element_kind("TextInput");
        // The field is focusable so the scene routes IME events to it, and it starts with an empty
        // Text payload: the edit runtime publishes the committed text and the caret into that
        // payload, and a node with no payload would be skipped (there is nothing to decorate).
        input->set_focusable(true);
        set_text(*input, "");
        input_label = input.get();

        card->add_child(std::move(title));
        card->add_child(std::move(count));
        card->add_child(std::move(button));
        card->add_child(std::move(input));
        root.add_child(std::move(card));
    }

    // The tap handler: bump the counter and repaint the label. A real app would bind a generated
    // setter; the shell has no generated component, so it writes the label directly.
    static void on_button_down(void* context, const tessera::ui::Event&) {
        auto* self = static_cast<ShellScene*>(context);
        ++self->taps;
        if (self->count_label != nullptr) {
            set_text(*self->count_label, "Taps: " + std::to_string(self->taps));
        }
    }

    tessera::ui::Node root{kRootId};
    // Borrowed from the tree, which owns it. Null until the constructor has built the tree.
    tessera::ui::Node* count_label = nullptr;
    // The text field, borrowed for the same reason: the edit runtime publishes the caret and the
    // committed text into it, and the tree owns it.
    tessera::ui::Node* input_label = nullptr;
    tessera::ui::UpdateScheduler scheduler;
    int taps = 0;
};

// The stylesheet. Every selector is a type selector on the element kinds above, which is what a
// real app writes today.
//
// The sizes are in logical pixels, so the same sheet lays out correctly at any density: the window
// reports its scale in `WindowMetrics::dpi_scale` and the layout engine converts. A phone is
// narrow, so the card fills the width.
inline tessera::ui::style::CompiledStyleSheet shell_stylesheet() {
    return tessera::ui::style::compile_stylesheet(R"(
View {
  box-sizing: border-box;
  width: 100%;
  display: flex;
  flex-direction: column;
  padding: 24px;
  background-color: #0b1220;
}
Text {
  font-size: 18px;
  color: #e2e8f0;
  margin-bottom: 12px;
}
Button {
  width: 160px;
  height: 48px;
  margin-bottom: 12px;
  border-radius: 12px;
  background-color: #2563eb;
}
Button:hover {
  background-color: #1d4ed8;
}
Button:active {
  background-color: #1e40af;
}
TextInput {
  box-sizing: border-box;
  width: 100%;
  height: 48px;
  padding: 12px;
  border-radius: 12px;
  background-color: #1e293b;
  color: #e2e8f0;
  font-size: 16px;
}
)");
}

} // namespace tessera::android_shell
