// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

using tessera::ui::Dirty;
using tessera::ui::FocusManager;
using tessera::ui::has_flag;
using tessera::ui::Node;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::has_state;
using tessera::ui::style::PseudoState;
using tessera::ui::style::SceneStyleTree;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "focus_style: " << message << "\n";
    }
    return condition;
}

std::string_view value_of(const ComputedStyle& style, const char* property) {
    const std::string* value = style.find(property);
    return value != nullptr ? std::string_view(*value) : std::string_view{};
}

} // namespace

int main() {
    Node root(1);
    auto first_ptr = std::make_unique<Node>(2);
    first_ptr->set_focusable(true);
    Node& first = root.add_child(std::move(first_ptr));

    auto second_ptr = std::make_unique<Node>(3);
    second_ptr->set_focusable(true);
    Node& second = root.add_child(std::move(second_ptr));

    const auto sheet = compile_stylesheet(R"(
*:focus { color: red; }
*:not(:focus) { color: blue; }
)");
    SceneStyleTree tree(root);
    FocusManager manager;
    manager.set_root(root);

    // The tree captures structure before focus changes; pseudo-state is refreshed per evaluation.
    first.clear_dirty(Dirty::Style | Dirty::Layout | Dirty::Paint | Dirty::Render);
    const auto* initial_view = tree.view(first);
    if (!expect(initial_view != nullptr, "first has a style view") ||
        !expect(initial_view->state == PseudoState::None, "initial pseudo-state is none") ||
        !expect(value_of(tree.compute(sheet, first), "color") == "blue",
                "unfocused resolves :not(:focus)") ||
        !expect(value_of(tree.compute(sheet, second), "color") == "blue",
                "sibling also resolves :not(:focus)")) {
        return 1;
    }

    if (!expect(manager.request_focus(&first), "focus first") ||
        !expect(first.pseudo_state() == PseudoState::Focus, "focus sets pseudo-state") ||
        !expect(has_state(tree.view(first)->state, PseudoState::Focus), "view exposes :focus") ||
        !expect(has_flag(first.dirty(), Dirty::Style), "focus marks Style dirty") ||
        !expect(value_of(tree.compute(sheet, first), "color") == "red",
                "focused re-evaluates to :focus style") ||
        !expect(value_of(tree.compute(sheet, second), "color") == "blue",
                "sibling stays :not(:focus)")) {
        return 2;
    }

    first.clear_dirty(Dirty::Style);
    if (!expect(manager.request_focus(&second), "focus second") ||
        !expect(!has_state(first.pseudo_state(), PseudoState::Focus), "old target blurs") ||
        !expect(has_state(second.pseudo_state(), PseudoState::Focus), "new target focuses") ||
        !expect(value_of(tree.compute(sheet, first), "color") == "blue",
                "blurred re-evaluates to :not(:focus)") ||
        !expect(value_of(tree.compute(sheet, second), "color") == "red",
                "new focus re-evaluates to :focus") ||
        !expect(has_flag(second.dirty(), Dirty::Style), "focus change marks new target dirty") ||
        !expect(has_flag(first.dirty(), Dirty::Style), "focus change marks old target dirty")) {
        return 3;
    }

    manager.clear_focus();
    if (!expect(second.pseudo_state() == PseudoState::None, "clear resets pseudo-state") ||
        !expect(value_of(tree.compute(sheet, second), "color") == "blue",
                "cleared focus resolves :not(:focus)") ||
        !expect(has_flag(second.dirty(), Dirty::Style), "clear marks Style dirty")) {
        return 4;
    }

    // :not(:focus) also matches nodes that can never receive focus (the root here).
    if (!expect(value_of(tree.compute(sheet, root), "color") == "blue",
                "non-focusable root resolves :not(:focus)")) {
        return 5;
    }

    std::cout << "focus_style: ok\n";
    return 0;
}
