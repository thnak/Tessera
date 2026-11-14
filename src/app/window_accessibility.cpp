// SPDX-License-Identifier: MIT
//
// WindowHost's accessibility wiring (issues #185 and #186, docs/specs/accessibility-v1.md section
// 12.3). This file is deliberately separate from window.cpp: it adapts the scene's paint style
// provider to the semantics builder's provider, supplies the snapshot source the controller builds
// from, and executes the drained actions on the scene (focus/press). window.cpp only constructs,
// ticks and tears the controller down, so the driver's own translation unit adds a handful of
// lines.

#include "window_impl.hpp"

#include <utility>

namespace tessera::app {
namespace {

namespace a11y = tessera::ui::a11y;

// Forwards the host's computed-style provider (the paint face of the same seam) to the
// accessibility
// builder's provider. One line of behaviour: the a11y builder declares the provider shape itself
// so
// the accessibility layer does not depend on paint/render, and this adapter is the join.
class PaintToA11yStyles final : public a11y::StyleProvider {
public:
    explicit PaintToA11yStyles(ui::paint::StyleProvider& styles)
        : styles_(styles) {}

    [[nodiscard]] ui::style::ComputedStyle compute(const ui::Node& node) override {
        return styles_.compute(node);
    }

private:
    ui::paint::StyleProvider& styles_;
};

// Depth-first lookup by stable node id. The scene is bounded (kMaxNodes in the snapshot), and this
// only runs for an action that assistive technology already validated against the snapshot.
ui::Node* find_node_by_id(ui::Node& node, ui::NodeId id) {
    if (node.id() == id) {
        return &node;
    }
    for (const std::unique_ptr<ui::Node>& child : node.children()) {
        if (ui::Node* found = find_node_by_id(*child, id); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

void WindowHost::Impl::start_accessibility() {
    // The controller lives in `accessibility` and is declared before `window` in window_impl.hpp,
    // so
    // it is destroyed first: its destructor uninstalls the UIA root and joins the bridge/session
    // while the platform window (and its native handle) is still valid.
    if (window == nullptr || host == nullptr) {
        return;
    }
    accessibility = std::make_unique<AccessibilityHost>();
    (void)accessibility->start(
        *window,
        [this](a11y::A11ySnapshot& out) {
            a11y::BuildOptions options;
            ui::FocusManager& focus = pump.router().focus_manager();
            if (focus.focused() != nullptr) {
                options.focused_id = focus.focused()->id();
            }
            PaintToA11yStyles styles(host->styles());
            (void)a11y::build_a11y_tree(root, styles, out, options);
        },
        [this](const AccessibilityAction& action) { apply_accessibility_action(action); });
}

void WindowHost::Impl::tick_accessibility(const ui::frame::FrameTick* frame) {
    if (accessibility == nullptr) {
        return;
    }
    // A style or layout pass can change roles, names, states and bounds without touching anything
    // the driver watches separately, so it is a semantics change. The controller itself is
    // dirty-only, so a static window still does no build, no publish and no bridge work.
    if (frame != nullptr && (frame->needs_style || frame->needs_layout)) {
        accessibility->mark_dirty();
    }
    (void)accessibility->tick();
}

void WindowHost::Impl::apply_accessibility_action(const AccessibilityAction& action) {
    ui::Node* target = find_node_by_id(root, action.node);
    if (target == nullptr) {
        return; // the node left the scene since the snapshot was published
    }
    // `Press` is the accessibility activation path (AT-SPI `DoAction`, UIA `Invoke`): it dispatches
    // the same Click a pointer or Enter/Space would, with ClickSource::Accessibility. `Focus` is
    // the
    // ordinary programmatic focus request, so screen-reader focus and keyboard focus stay one
    // state.
    // SetValue/Scroll requests are not wired in this slice (the spec's known-limits list); they are
    // dropped rather than faked.
    if (a11y::has_action(action.action, a11y::Action::Press)) {
        (void)pump.router().invoke(root, *target);
        loop->mark_render_dirty();
        return;
    }
    if (a11y::has_action(action.action, a11y::Action::Focus)) {
        if (pump.router().focus_manager().request_focus(target)) {
            loop->mark_render_dirty();
        }
    }
}

} // namespace tessera::app
