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
#include "tessera/ui/layout/table_span.hpp"
#include "tessera/ui/paint/list_marker_inside.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/structure_epoch.hpp"
#include "tessera/ui/scene/text_wrap.hpp"
#include "tessera/ui/text/size_runs.hpp"
#include "tessera/ui/text/text_measure.hpp"

namespace tessera::app {

namespace {

// The dirt that can change a layout input: its style, its layout flags, or the text and wrap its
// paint payload carries.
constexpr ui::Dirty kInputDirt = ui::Dirty::Style | ui::Dirty::Layout | ui::Dirty::Paint;

// A retained input that this pass may keep as it is: it was built for this node, nothing under the
// node is dirty, and no restyle root is on or above it.
bool keep_input(const ui::Node& node, const ui::layout::LayoutInput& input,
                const LayoutInputReuse& reuse, bool on_path) {
    if (reuse.force_all || input.subtree_signature == 0 || input.id != node.id() ||
        ui::has_any(node.subtree_dirty(), kInputDirt)) {
        return false;
    }
    return !on_path || (!reuse.roots.contains(&node) && !reuse.path.contains(&node));
}

// The layout measurer as the advance seam of a list marker's counter text (the wrap's own seam).
float marker_advance(std::string_view text, const ui::text::TextStyle& style, const void* user) {
    ui::text::TextConstraints one_line;
    one_line.wrap = false;
    return static_cast<const ui::text::TextMeasurer*>(user)->measure(text, style, one_line).width;
}

// One layout tree of computed styles (the paint pass resolves the same provider, so both agree),
// written into `input` in place. The tree is retained between passes (issue #701): clearing a
// `std::vector<LayoutInput>` would free every nested children vector, so a pass reuses the elements
// it finds and only resizes at the end. On a stable tree that makes the pass allocation-free for
// the structure (one 176-byte block per container per pass before), and it is also what keeps the
// MSVC `std::map` move from running at all.
//
// A child whose retained input `keep_input` accepts is not visited; its stored signature joins the
// parent's. `rebuild` is set below a restyle root or a restyled node, whose descendants inherit.
// `on_path` is true when `node` is a restyle root or one of its ancestors.
void fill_layout_input(const ui::Node& node, ui::paint::StyleProvider& styles,
                       const SafeAreaState& safe_area, ui::layout::LayoutInput& input,
                       ui::layout::LayoutSignatureBuilder& signature, const LayoutInputReuse& reuse,
                       bool on_path, bool rebuild, const ui::text::TextMeasurer& measurer) {
    input.id = node.id();
    input.text.clear();
    input.text_wrap = false;
    input.style = styles.resolve(node);
    ui::layout::apply_table_spans(input, node);
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
    input.text_runs = {};
    input.text_indent_q = 0;
    if (text_payload) {
        // The first Text of a `list-style-position: inside` item reserves the marker's box.
        if (const auto marker =
                ui::paint::inside_marker(node, styles, &marker_advance, &measurer)) {
            input.text_indent_q = static_cast<std::uint16_t>(std::lround(marker->width * 16.0f));
        }
    }
    if (text_payload && !paint->spans.empty()) {
        // A span may shrink inside its paragraph (`sub`, `sup`, `small`): the measure must see
        // the runs paint will draw (docs/design/inline-size-runs.md).
        std::vector<ui::text::SpanFontSize> sizes;
        sizes.reserve(paint->spans.size());
        for (const ui::PaintSpan& span : paint->spans) {
            for (const auto& child : node.children()) {
                if (child->id() != span.node_id) {
                    continue;
                }
                const ui::style::ComputedStyle& span_style = styles.resolve(*child);
                sizes.push_back(
                    {span.begin, span.end, ui::text::text_style_from(span_style).font_size});
                break;
            }
        }
        input.text_runs =
            ui::text::size_runs_from_spans(sizes, ui::text::text_style_from(input.style).font_size);
    }
    // The layout signature is folded in here, while the node is hot, rather than by a second walk
    // over the finished tree: that walk was a cache miss per node.
    signature.enter(input);
    const bool below_rebuild = rebuild || ui::has_flag(node.dirty(), ui::Dirty::Style) ||
                               (on_path && reuse.roots.contains(&node));
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
        ui::layout::LayoutInput& slot = input.children[used];
        if (!below_rebuild && keep_input(*child, slot, reuse, on_path)) {
            signature.reuse(slot.subtree_signature);
        } else {
            const bool child_on_path =
                on_path && !below_rebuild &&
                (reuse.roots.contains(child.get()) || reuse.path.contains(child.get()));
            fill_layout_input(*child, styles, safe_area, slot, signature, reuse, child_on_path,
                              below_rebuild, measurer);
        }
        ++used;
    }
    input.children.resize(used);
    input.subtree_signature = signature.leave(used);
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

void AppHost::Impl::run_layout_pass(ui::paint::StyleProvider& styles, bool incremental) {
    const ui::layout::LayoutViewport viewport{config.width, config.height, config.dpi_scale};
    ui::layout::LayoutSignatureBuilder signature(viewport);
    // Which subtrees the retained input can keep (layout_input_reuse.hpp). Anything the dirty flags
    // cannot vouch for rebuilds the whole input, which is what every pass did before.
    LayoutInputReuse& reuse = input_reuse;
    reuse.roots.clear();
    reuse.path.clear();
    reuse.force_all = reuse.stale || !incremental || tree == nullptr || !sheet.subtree_local() ||
                      tree->built_for_structure_epoch() != ui::scene_structure_epoch() ||
                      reuse.structure_epoch != ui::scene_structure_epoch() ||
                      !same_safe_area(gated_metrics, gated_metrics_valid, safe_area) ||
                      tree->restyled_roots().size() > LayoutInputReuse::kMaxRoots;
    if (!reuse.force_all) {
        for (const ui::Node* restyled : tree->restyled_roots()) {
            reuse.roots.insert(restyled);
            for (const ui::Node* up = restyled->parent();
                 up != nullptr && reuse.path.insert(up).second; up = up->parent()) {
            }
        }
    }
    const bool root_on_path = reuse.roots.contains(&root) || reuse.path.contains(&root);
    if (keep_input(root, layout_input, reuse, !reuse.roots.empty())) {
        signature.reuse(layout_input.subtree_signature);
    } else {
        static const ui::text::StubTextMeasurer stub_measurer;
        const ui::text::TextMeasurer& measurer =
            config.text_measurer != nullptr ? *config.text_measurer : stub_measurer;
        fill_layout_input(root, styles, safe_area, layout_input, signature, reuse, root_on_path,
                          false, measurer);
    }
    reuse.stale = false;
    reuse.built_this_frame = true;
    reuse.structure_epoch = ui::scene_structure_epoch();
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

void AppHost::Impl::note_frame_end() {
    // The flags are about to be cleared. If dirt the layout input never saw goes with them, the
    // retained input is behind the scene and the next pass must rebuild it whole.
    if (!input_reuse.built_this_frame && ui::has_any(root.subtree_dirty(), kInputDirt)) {
        input_reuse.stale = true;
    }
    input_reuse.built_this_frame = false;
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
