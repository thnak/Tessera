// SPDX-License-Identifier: MIT

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <iostream>
#include <memory>

namespace {

using tessera::ui::Dirty;
using tessera::ui::FocusManager;
using tessera::ui::has_flag;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::key::kTab;
using tessera::ui::style::has_state;
using tessera::ui::style::PseudoState;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "focus_policy: " << message << "\n";
    }
    return condition;
}

Node& add(Node& parent, std::unique_ptr<Node> child) {
    return parent.add_child(std::move(child));
}

InputEvent pointer(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    return event;
}

} // namespace

int main() {
    // Tab order: positive tab_index ascending (ties in tree order), then zero in tree order;
    // negative values skip sequential traversal.
    Node root(1);
    auto a_ptr = std::make_unique<Node>(2);
    a_ptr->set_focusable(true);
    Node& a = add(root, std::move(a_ptr));

    Node& panel = add(root, std::make_unique<Node>(3));

    auto b_ptr = std::make_unique<Node>(4);
    b_ptr->set_focusable(true);
    Node& b = add(panel, std::move(b_ptr));

    auto c_ptr = std::make_unique<Node>(5);
    c_ptr->set_focusable(true);
    c_ptr->set_tab_index(-1);
    Node& c = add(panel, std::move(c_ptr));

    auto d_ptr = std::make_unique<Node>(6);
    d_ptr->set_focusable(true);
    d_ptr->set_tab_index(2);
    Node& d = add(panel, std::move(d_ptr));

    auto e_ptr = std::make_unique<Node>(7);
    e_ptr->set_focusable(true);
    e_ptr->set_tab_index(1);
    Node& e = add(panel, std::move(e_ptr));

    auto f_ptr = std::make_unique<Node>(8);
    f_ptr->set_focusable(true);
    f_ptr->set_tab_index(1);
    Node& f = add(root, std::move(f_ptr));

    auto g_ptr = std::make_unique<Node>(9);
    g_ptr->set_focusable(true);
    Node& g = add(root, std::move(g_ptr));

    Node& hidden = add(root, std::make_unique<Node>(10));
    hidden.set_visible(false);
    auto hidden_child_ptr = std::make_unique<Node>(11);
    hidden_child_ptr->set_focusable(true);
    hidden_child_ptr->set_tab_index(5);
    Node& hidden_child = add(hidden, std::move(hidden_child_ptr));

    FocusManager manager;
    manager.set_root(root);
    if (!expect(a.tab_index() == 0, "tab_index defaults to zero") ||
        !expect(c.tab_index() == -1, "tab_index stores negative values") ||
        !expect(manager.focused() == nullptr, "new manager has no focus")) {
        return 1;
    }

    // Pre-order focusables: a(0), b(0), c(-1), d(2), e(1), f(1), g(0). Positive order is e, f, d.
    if (!expect(manager.focus_next(root), "first tab stops at positive tab_index") ||
        !expect(manager.focused() == &e, "tab first is e") ||
        !expect(manager.focus_next(root), "second positive") ||
        !expect(manager.focused() == &f, "equal positive keeps tree order") ||
        !expect(manager.focus_next(root), "third positive") ||
        !expect(manager.focused() == &d, "larger positive comes later") ||
        !expect(manager.focus_next(root), "first zero") ||
        !expect(manager.focused() == &a, "zero follows positives in tree order") ||
        !expect(manager.focus_next(root), "second zero") ||
        !expect(manager.focused() == &b, "second zero is b") ||
        !expect(manager.focus_next(root), "third zero") ||
        !expect(manager.focused() == &g, "third zero is g") ||
        !expect(manager.focus_next(root), "wrap after zero") ||
        !expect(manager.focused() == &e, "wrap to first positive") ||
        !expect(manager.focus_previous(root), "reverse wraps") ||
        !expect(manager.focused() == &g, "reverse wraps to last zero") ||
        !expect(manager.focus_previous(root), "reverse zero") ||
        !expect(manager.focused() == &b, "reverse zero is b")) {
        return 2;
    }

    // Negative tab_index is skipped by Tab but reachable programmatically.
    if (!expect(manager.request_focus(&c), "negative tab_index is programmatically focusable") ||
        !expect(manager.focused() == &c, "negative focus target") ||
        !expect(has_state(c.pseudo_state(), PseudoState::Focus), "negative target gets :focus") ||
        !expect(has_flag(c.dirty(), Dirty::Style), "focus marks Style dirty") ||
        !expect(!manager.request_focus(&hidden_child), "hidden positive tab_index cannot focus") ||
        !expect(manager.focus_next(root), "next from skipped node starts at first candidate") ||
        !expect(manager.focused() == &e, "next from negative starts at first positive")) {
        return 3;
    }
    if (!expect(manager.request_focus(&c), "re-focus negative") ||
        !expect(manager.focus_previous(root), "previous from skipped node") ||
        !expect(manager.focused() == &g, "previous from negative starts at last candidate")) {
        return 4;
    }

    // Modal trapping: focus is confined to the dialog subtree, including pointer focus.
    Node modal_scene(20);
    modal_scene.set_bounds({0, 0, 500, 500});

    auto outside_ptr = std::make_unique<Node>(21);
    outside_ptr->set_focusable(true);
    outside_ptr->set_bounds({300, 300, 50, 50});
    Node& outside = add(modal_scene, std::move(outside_ptr));

    auto dialog_ptr = std::make_unique<Node>(22);
    dialog_ptr->set_bounds({0, 0, 200, 200});
    Node& dialog = add(modal_scene, std::move(dialog_ptr));

    auto first_ptr = std::make_unique<Node>(23);
    first_ptr->set_focusable(true);
    first_ptr->set_bounds({10, 10, 50, 50});
    Node& first = add(dialog, std::move(first_ptr));

    auto second_ptr = std::make_unique<Node>(24);
    second_ptr->set_focusable(true);
    second_ptr->set_bounds({70, 10, 50, 50});
    Node& second = add(dialog, std::move(second_ptr));

    auto outside2_ptr = std::make_unique<Node>(25);
    outside2_ptr->set_focusable(true);
    outside2_ptr->set_bounds({380, 300, 50, 50});
    Node& outside2 = add(modal_scene, std::move(outside2_ptr));

    FocusManager modal_manager;
    modal_manager.set_root(modal_scene);
    if (!expect(modal_manager.request_focus(&outside), "pre-modal focus") ||
        !expect(has_state(outside.pseudo_state(), PseudoState::Focus), "pre-modal :focus") ||
        !expect(modal_manager.set_modal_root(&dialog), "install modal trap") ||
        !expect(modal_manager.modal_root() == &dialog, "modal root reported") ||
        !expect(modal_manager.focused() == &first, "modal trap focuses first candidate") ||
        !expect(!has_state(outside.pseudo_state(), PseudoState::Focus), "outside loses :focus") ||
        !expect(has_state(first.pseudo_state(), PseudoState::Focus), "inside gains :focus") ||
        !expect(!modal_manager.allows_focus(outside), "outside focus is blocked") ||
        !expect(modal_manager.allows_focus(second), "inside focus is allowed")) {
        return 5;
    }

    if (!expect(modal_manager.focus_next(modal_scene), "tab inside modal") ||
        !expect(modal_manager.focused() == &second, "tab stays inside modal") ||
        !expect(modal_manager.focus_next(modal_scene), "tab wraps inside modal") ||
        !expect(modal_manager.focused() == &first, "tab wraps to first inside") ||
        !expect(!modal_manager.request_focus(&outside), "outside request rejected by trap") ||
        !expect(!modal_manager.request_focus(&outside2), "second outside request rejected") ||
        !expect(modal_manager.focused() == &first, "rejected requests keep focus")) {
        return 6;
    }

    if (!expect(modal_manager.request_focus(&second), "focus second inside")) {
        return 7;
    }

    modal_manager.clear_modal_root();
    if (!expect(modal_manager.modal_root() == nullptr, "trap cleared") ||
        !expect(modal_manager.focused() == &outside, "saved focus restored") ||
        !expect(has_state(outside.pseudo_state(), PseudoState::Focus), "restored :focus") ||
        !expect(!has_state(second.pseudo_state(), PseudoState::Focus), "inside loses :focus") ||
        !expect(modal_manager.focus_next(modal_scene), "tab after release") ||
        !expect(modal_manager.focused() == &first, "tab unrestricted after release")) {
        return 8;
    }

    // Clearing does not clobber focus when the saved node is no longer valid.
    if (!expect(modal_manager.request_focus(&outside), "pre-modal focus again") ||
        !expect(modal_manager.set_modal_root(&dialog), "second trap") ||
        !expect(modal_manager.focused() == &first, "second trap focuses first")) {
        return 9;
    }
    outside.set_visible(false);
    modal_manager.clear_modal_root();
    outside.set_visible(true);
    if (!expect(modal_manager.modal_root() == nullptr, "trap cleared with invalid saved focus") ||
        !expect(modal_manager.focused() == &first, "invalid saved focus leaves current focus")) {
        return 10;
    }

    // Detached nodes cannot become modal roots; nullptr clears the trap.
    Node detached(90);
    if (!expect(!modal_manager.set_modal_root(&detached), "detached modal root rejected") ||
        !expect(modal_manager.set_modal_root(&dialog), "reinstall trap") ||
        !expect(modal_manager.set_modal_root(nullptr), "nullptr clears trap") ||
        !expect(modal_manager.modal_root() == nullptr, "nullptr cleared modal root")) {
        return 11;
    }

    // Pointer focus respects the trap.
    InputRouter router;
    router.focus_manager().set_root(modal_scene);
    if (!expect(router.focus_manager().request_focus(&outside), "router pre-modal focus") ||
        !expect(router.focus_manager().set_modal_root(&dialog), "router trap") ||
        !expect(router.focus_manager().focused() == &first, "router trap focus")) {
        return 12;
    }
    const auto outside_down =
        router.dispatch(modal_scene, pointer(InputEventType::PointerDown, 320, 320));
    if (!expect(outside_down.dispatched, "outside pointer down dispatches") ||
        !expect(outside_down.target == &outside, "outside pointer target") ||
        !expect(router.focus_manager().focused() == &first,
                "pointer outside the trap does not steal focus")) {
        return 13;
    }

    const auto blank_down =
        router.dispatch(modal_scene, pointer(InputEventType::PointerDown, 450, 450));
    if (!expect(blank_down.dispatched, "blank pointer down dispatches") ||
        !expect(blank_down.target == &modal_scene, "blank pointer target is the scene root") ||
        !expect(router.focus_manager().focused() == &first,
                "blank pointer down does not clear trapped focus")) {
        return 14;
    }

    const auto inside_down =
        router.dispatch(modal_scene, pointer(InputEventType::PointerDown, 80, 20));
    if (!expect(inside_down.dispatched, "inside pointer down dispatches") ||
        !expect(inside_down.target == &second, "inside pointer target") ||
        !expect(router.focus_manager().focused() == &second, "inside pointer moves focus")) {
        return 15;
    }

    // Tab inside the modal trap is keyboard modality: it sets :focus-visible on the traversal
    // target, and a following pointer press clears it again.
    InputEvent tab;
    tab.type = InputEventType::KeyDown;
    tab.key = kTab;
    const auto tabbed = router.dispatch(modal_scene, tab);
    if (!expect(tabbed.focus_moved, "tab inside modal moves focus") ||
        !expect(router.focus_manager().focused() == &first, "tab wraps to first inside") ||
        !expect(has_state(first.pseudo_state(), PseudoState::Focus) &&
                    has_state(first.pseudo_state(), PseudoState::FocusVisible),
                "tab sets :focus-visible inside the trap")) {
        return 16;
    }
    const auto after_tab_down =
        router.dispatch(modal_scene, pointer(InputEventType::PointerDown, 80, 20));
    if (!expect(after_tab_down.target == &second, "pointer press hits second") ||
        !expect(router.focus_manager().focused() == &second, "pointer press moves focus") ||
        !expect(has_state(second.pseudo_state(), PseudoState::Focus) &&
                    !has_state(second.pseudo_state(), PseudoState::FocusVisible),
                "pointer press clears :focus-visible")) {
        return 17;
    }

    router.reset();
    if (!expect(router.captured() == nullptr, "reset cleared capture") ||
        !expect(router.focus_manager().focused() == nullptr, "reset cleared focus") ||
        !expect(router.focus_manager().modal_root() == nullptr, "reset cleared modal trap")) {
        return 18;
    }

    // Reusing the router across scene roots applies the same modality rule to the new root.
    Node other_root(40);
    other_root.set_bounds({0, 0, 100, 100});
    auto other_focus_ptr = std::make_unique<Node>(41);
    other_focus_ptr->set_focusable(true);
    other_focus_ptr->set_bounds({10, 10, 50, 50});
    Node& other_focus = add(other_root, std::move(other_focus_ptr));
    const auto second_root_tab = router.dispatch(other_root, tab);
    if (!expect(second_root_tab.focus_moved, "tab on the second root moves focus") ||
        !expect(other_focus.pseudo_state() == (PseudoState::Focus | PseudoState::FocusVisible),
                "second root focus gets :focus-visible")) {
        return 19;
    }
    const auto second_root_down =
        router.dispatch(other_root, pointer(InputEventType::PointerDown, 20, 20));
    if (!expect(second_root_down.target == &other_focus, "pointer hits the second root target") ||
        !expect(has_state(other_focus.pseudo_state(), PseudoState::Focus) &&
                    !has_state(other_focus.pseudo_state(), PseudoState::FocusVisible),
                "pointer focus on the second root clears :focus-visible")) {
        return 20;
    }

    // Removing the modal root releases the trap without dereferencing it.
    if (!expect(modal_manager.request_focus(&outside), "pre-modal focus for removal") ||
        !expect(modal_manager.set_modal_root(&dialog), "trap for removal") ||
        !expect(modal_manager.focused() == &first, "trap focus for removal")) {
        return 21;
    }
    std::unique_ptr<Node> removed = modal_scene.remove_child(dialog);
    if (!expect(removed != nullptr, "dialog removed") ||
        !expect(modal_manager.modal_root() == nullptr, "removed modal root no longer resolves")) {
        return 22;
    }
    // Destroy the subtree before the manager notices; id resolution must not dereference it.
    removed.reset();
    modal_manager.clear_modal_root();
    if (!expect(modal_manager.focused() == &outside, "destroyed modal restores saved focus")) {
        return 23;
    }

    std::cout << "focus_policy: ok\n";
    return 0;
}
