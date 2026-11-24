// SPDX-License-Identifier: MIT
//
// The layout-unchanged gate (issue #735, slice A). Split out of host.cpp, which is at its size
// budget.
//
// The layout pass costs ~27% of a class-change frame at 6k nodes, and almost all of it is
// `build_layout_input` (one `LayoutInput` per node, each holding a `ComputedStyle` and a
// `std::string`) plus the signature hash over the result. When a restyle provably changed no layout
// input, all of that is wasted work: the previous `LayoutResult` is still exactly what a reflow
// would produce.
//
// Every layout input is covered, and each one that no computed style carries is checked here:
//   * the layout-affecting longhands and the text - `restyled_layout_changed` compares the layout
//     digest of every restyled node before and after (a paint-only change leaves it identical);
//   * the tree shape - the scene's structure epoch, the same signal the selector tree uses;
//   * the viewport - width, height and DPI scale, compared against the last pass;
//   * the safe-area insets - the resolved metrics, which `apply_safe_area_style` folds into the
//     padding/margin of a SafeArea node without any style edit.
// Anything it cannot prove falls back to a real pass, so a wrong answer is impossible by
// construction: the gate only ever answers "unchanged" when every input is known to be equal.

#include "host_impl.hpp"
#include "tessera/ui/layout/layout_signature.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/structure_epoch.hpp"
#include "tessera/ui/scene/text_wrap.hpp"

namespace tessera::app {

namespace {

// One layout tree of computed styles (the paint pass resolves the same provider, so both agree),
// written into `input` in place. The tree is retained between passes (issue #701): clearing a
// `std::vector<LayoutInput>` would free every nested children vector, so a pass reuses the elements
// it finds and only resizes at the end. On a stable tree that makes the pass allocation-free for
// the structure (one 176-byte block per container per pass before), and it is also what keeps the
// MSVC `std::map` move from running at all.
void fill_layout_input(const ui::Node& node, ui::paint::StyleProvider& styles,
                       const SafeAreaState& safe_area, ui::layout::LayoutInput& input,
                       ui::layout::LayoutSignatureBuilder& signature) {
    input.id = node.id();
    input.text.clear();
    input.text_wrap = false;
    input.style = styles.resolve(node);
    // A SafeArea node's padding or margin is host-owned, and the resolution is the SAME function
    // the design-time preview calls (issue #408): the app's metrics come from the OS and the
    // preview's from a simulated posture, but the token parsing and the padding writer are one
    // implementation.
    (void)ui::style::apply_safe_area_style(input.style, node, safe_area.metrics_or_null());
    const auto& paint = node.paint_data();
    const bool text_payload = paint && paint->kind == ui::PaintKind::Text;
    if (text_payload) {
        input.text = paint->text;
        // Layout's text-fit fact must use paint's own wrap decision (issue #490).
        input.text_wrap = ui::paint_wraps(node.element_kind(), *paint);
    }
    // The layout signature is folded in here, while the node is hot, rather than by a second walk
    // over the finished tree: that walk was a cache miss per node.
    signature.enter(input);
    // Growing the vector would move each LayoutInput, and the MSVC `std::map` move constructor is
    // not `noexcept`: the vector then copies every element, subtree and 101-entry style included.
    // Measured at ~330 allocations per node (issue #711). Reserving up front means a grow never
    // happens mid-walk; an existing element is reused, a missing one is appended.
    if (input.children.capacity() < node.children().size()) {
        input.children.reserve(node.children().size());
    }
    std::size_t used = 0;
    for (const auto& child : node.children()) {
        // Inline spans are runs of this text, not boxes (issue #229, elements-basic.md 2.1).
        if (text_payload && paint->has_span(child->id())) {
            continue;
        }
        if (used == input.children.size()) {
            input.children.emplace_back();
        }
        fill_layout_input(*child, styles, safe_area, input.children[used], signature);
        ++used;
    }
    input.children.resize(used);
    signature.leave(used);
}

// True when the safe-area snapshot the last layout pass resolved is still the one in force. The
// insets reach layout through `apply_safe_area_style`, which rewrites a SafeArea node's padding or
// margin from the metrics without any style edit, so they are a layout input no computed style
// carries. Only the fields the inset arithmetic reads are compared: the window extent is already
// covered by the viewport check, and the flags (focused, occluded, minimized) never reach layout.
bool same_safe_area(const hal::WindowMetrics& before, bool before_valid, const SafeAreaState& now) {
    if (before_valid != now.has_metrics()) {
        return false;
    }
    if (!before_valid) {
        return true; // no metrics before and none now: nothing to compare
    }
    const hal::WindowMetrics& after = now.current();
    const auto same_insets = [](const hal::WindowInsets& a, const hal::WindowInsets& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    };
    if (!same_insets(before.insets, after.insets) ||
        !same_insets(before.ime_insets, after.ime_insets) ||
        !same_insets(before.gesture_insets, after.gesture_insets)) {
        return false;
    }
    if (before.cutouts.count != after.cutouts.count ||
        !same_insets(before.cutouts.waterfall_insets, after.cutouts.waterfall_insets)) {
        return false;
    }
    for (std::uint8_t i = 0; i < before.cutouts.count && i < hal::kMaxCutouts; ++i) {
        const hal::WindowRect& a = before.cutouts.bounds[i];
        const hal::WindowRect& b = after.cutouts.bounds[i];
        if (a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height) {
            return false;
        }
    }
    // The hinge is geometry a foldable's SafeArea resolves against.
    if (before.fold.state != after.fold.state ||
        before.fold.orientation != after.fold.orientation ||
        before.fold.separating != after.fold.separating) {
        return false;
    }
    const hal::WindowRect& hinge_a = before.fold.hinge_bounds;
    const hal::WindowRect& hinge_b = after.fold.hinge_bounds;
    return hinge_a.x == hinge_b.x && hinge_a.y == hinge_b.y && hinge_a.width == hinge_b.width &&
           hinge_a.height == hinge_b.height;
}

} // namespace

void AppHost::Impl::run_layout_pass(ui::paint::StyleProvider& styles) {
    const ui::layout::LayoutViewport viewport{config.width, config.height, config.dpi_scale};
    ui::layout::LayoutSignatureBuilder signature(viewport);
    fill_layout_input(root, styles, safe_area, layout_input, signature);
    const ui::layout::LayoutInput& input = layout_input;
    // The scheduler reuses the cached result when the layout input signature (tree shape,
    // layout-affecting longhands, text, viewport) is unchanged, so a style-only change that
    // cannot move boxes skips Yoga and only re-syncs the scene style.
    last_layout = layout_scheduler.update(input, viewport, true, signature.value());
    last_applied = ui::layout::apply_layout(last_layout, root, resized_nodes);
    // Remember the inputs the gate compares against next frame. `apply_layout` is what writes the
    // boxes, so a reused result still has to be applied once - the gate skips building the input
    // and hashing it, not the (cheap, already-in-tree-order) box write.
    gated_viewport = viewport;
    gated_metrics = safe_area.current();
    gated_metrics_valid = safe_area.has_metrics();
}

bool AppHost::Impl::scoped_sync_allowed(bool incremental) const {
    if (!incremental || tree == nullptr || !sheet.subtree_local() ||
        tree->built_for_structure_epoch() != ui::scene_structure_epoch()) {
        return false;
    }
    const ui::layout::LayoutViewport viewport{config.width, config.height, config.dpi_scale};
    return viewport.width == gated_viewport.width && viewport.height == gated_viewport.height &&
           viewport.dpi_scale == gated_viewport.dpi_scale &&
           same_safe_area(gated_metrics, gated_metrics_valid, safe_area);
}

bool AppHost::Impl::layout_unchanged(bool incremental) {
    const auto decline = [this](LayoutGate reason) {
        layout_gate = reason;
        return false;
    };
    // A rebuilt selector tree has no restyle record: every node's style is recomputed from scratch
    // and `restyled_` is empty, which would read as "nothing changed". Decline instead.
    if (!incremental) {
        return decline(LayoutGate::TreeRebuilt);
    }
    if (!layout_scheduler.valid() || !layout_scheduler.has_signature()) {
        return decline(LayoutGate::NoPreviousLayout);
    }
    // A node that asked for layout explicitly (a scroll, a virtual-list window, a SafeArea mark)
    // is a report the gate must honour even when the signature would match: the caller knows
    // something the digest cannot see.
    if (ui::has_flag(root.subtree_dirty(), ui::Dirty::Layout)) {
        return decline(LayoutGate::LayoutDirt);
    }
    if (tree == nullptr || tree->built_for_structure_epoch() != ui::scene_structure_epoch()) {
        return decline(LayoutGate::ShapeChanged);
    }
    if (!sheet.subtree_local()) {
        return decline(LayoutGate::NotSubtreeLocal);
    }
    const ui::layout::LayoutViewport viewport{config.width, config.height, config.dpi_scale};
    if (viewport.width != gated_viewport.width || viewport.height != gated_viewport.height ||
        viewport.dpi_scale != gated_viewport.dpi_scale) {
        return decline(LayoutGate::ViewportChanged);
    }
    if (!same_safe_area(gated_metrics, gated_metrics_valid, safe_area)) {
        return decline(LayoutGate::SafeAreaChanged);
    }
    // The restyle half: recompute only the restyled nodes and compare their layout digests. This
    // is also what fills the memo the sync and paint passes then read, so it is not extra work.
    if (tree->restyled_layout_changed(sheet)) {
        return decline(LayoutGate::StyleMoved);
    }
    layout_gate = LayoutGate::None;
    return true;
}

} // namespace tessera::app
