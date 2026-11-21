// SPDX-License-Identifier: MIT

#include "scene_build.hpp"
#include "scene_layout.hpp"
#include "scene_session.hpp"
#include "scene_session_slots.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;
namespace hal = tessera::hal;

// The fixed step between events of one gesture. Deterministic on purpose: the preview owns no
// clock, so one script produces the same timestamps on every run and every platform.
constexpr double kEventStepSeconds = 1.0 / 60.0;

} // namespace

ui::InputEvent to_input_event(const PreviewInput& input, double timestamp_s) {
    ui::InputEvent event;
    switch (input.kind) {
    case PreviewInput::Kind::PointerMove:
        event.type = ui::InputEventType::PointerMove;
        break;
    case PreviewInput::Kind::PointerDown:
        event.type = ui::InputEventType::PointerDown;
        break;
    case PreviewInput::Kind::PointerUp:
        event.type = ui::InputEventType::PointerUp;
        break;
    case PreviewInput::Kind::PointerCancel:
        event.type = ui::InputEventType::PointerCancel;
        break;
    case PreviewInput::Kind::KeyDown:
        event.type = ui::InputEventType::KeyDown;
        break;
    case PreviewInput::Kind::KeyUp:
        event.type = ui::InputEventType::KeyUp;
        break;
    }
    event.timestamp_s = timestamp_s;
    event.x = input.x;
    event.y = input.y;
    event.pointer_id = input.pointer_id;
    event.pointer_kind = ui::PointerKind::Mouse;
    // A press or release is a primary left-button contact; a move carries no button. That is what
    // the native harness synthesizes, and what ClickSynthesizer needs to read the pair as a click.
    const bool contact = input.kind == PreviewInput::Kind::PointerDown ||
                         input.kind == PreviewInput::Kind::PointerUp;
    event.button = contact ? ui::PointerButton::Left : ui::PointerButton::None;
    event.is_primary = true;
    event.key = input.key;
    event.repeat = input.repeat;
    event.modifiers = static_cast<ui::Modifier>(input.modifiers & 0x3FU);
    return event;
}

PreviewSession::PreviewSession(const ir::IrModule& module, const ir::Component& component,
                               const ui::style::CompiledStyleSheet& sheet, Options options)
    : module_(module),
      component_(component),
      // A session given the media-independent form resolves it itself against the viewport it was
      // asked for (issue #436), so a caller cannot hand it a sheet compiled for a different
      // rectangle. A caller that passes a compiled sheet keeps it, which is every pre-#436 caller.
      sheet_(options.prepared.has_value() ? options.prepared->resolve(options.media) : sheet),
      options_(std::move(options)),
      media_(options_.media) {
    layout_metrics_.width = options_.width;
    layout_metrics_.height = options_.height;
    layout_metrics_.dpi_scale = options_.dpi_scale;
    layout_metrics_.posture = options_.posture;
    layout_metrics_.insets = options_.insets;
    layout_metrics_.cutouts = options_.cutouts;
    // The settle loop's relayout hook (issue #693 decision 1): a round that delivered an
    // `onSizeChange` callback which changed state must re-evaluate the tree, not just re-lay it
    // out. The preview's "re-evaluate" is `rebuild()`, which is also what a dispatch does after a
    // handler ran.
    settle_.set_relayout(&PreviewSession::settle_relayout, this);
    if (options_.carried != nullptr) {
        const std::vector<std::string> only_this{component.component_id};
        carry_report_ = apply_state(
            component, *options_.carried,
            options_.live_component_ids != nullptr ? *options_.live_component_ids : only_this,
            scope_);
    }
    rebuild();
}

PreviewSession::~PreviewSession() = default;

void PreviewSession::rebuild() {
    // Keyboard focus survives a rebuild (issue #388): activating a button changes state, the tree
    // is rebuilt, and a person who pressed Enter expects to be able to press it again. The focus
    // manager holds a raw pointer into the old tree, so what is kept is the node's automation id --
    // assigned in tree order, so the same structure gives the same id -- and whether the ring was
    // showing.
    std::uint64_t kept_focus = 0;
    bool kept_ring = false;
    if (const ui::Node* focused = router_.focus_manager().focused()) {
        kept_focus = focused->automation_id();
        kept_ring =
            ui::style::has_state(focused->pseudo_state(), ui::style::PseudoState::FocusVisible);
    }
    // The router holds raw capture/hover/active pointers into the tree about to be replaced and the
    // focus manager resolves ids under the old root, so both must drop that state before the old
    // tree dies (input_dispatch.hpp documents the same ordering for every caller).
    router_.reset();
    slots_.clear();
    // The observations target nodes in the tree about to be replaced, so they are detached before
    // it dies (issue #694), for the same reason the router and the behaviors are.
    detach_size_observations();
    // The behaviors target nodes in the tree about to be replaced, so they are detached before it
    // dies (the same ordering the router needs, and for the same reason).
    detach_behaviors();
    behaviors_.clear();
    behavior_errors_.clear();
    root_ = std::make_unique<ui::Node>(1);
    // The old tree is gone, so what was applied to it is gone too; the layer is re-applied below.
    applied_overrides_.clear();
    override_text_.clear();

    const SceneBuildResult built = build_scene(component_, *root_, scope_);
    built_ = built.error.empty();
    build_error_ = built.error;
    // The spans belong to the tree that was just built, so they are replaced with it - including
    // with an empty list when the build failed, because a span for a node that is not on screen
    // would let a commit edit a range nothing is showing.
    node_sources_ = built.sources;
    if (!built_) {
        // A tree that cannot be built installs no handlers, so a page sees an empty snapshot and
        // reports the same unsupported construct the P2 preview reports.
        return;
    }
    for (const SceneBuildResult::SceneEventHandler& handler : built.handlers) {
        bind_handler(handler.automation_id, handler.event, handler.handler);
    }

    // Before layout: an override can move a box, so the pass must see it.
    apply_overrides();
    ui::layout::LayoutEngine engine(options_.measurer);
    const SceneLayoutResult laid_out =
        layout_scene(*root_, sheet_, engine, layout_metrics_, options_.measurer, &settle_);
    metrics_ = laid_out.metrics;
    // The `onSizeChange` observations attach AFTER the first layout (issue #694), so their first
    // delivery carries the size the tree actually has rather than 0x0.
    attach_size_observations(built);
    // ADR-041 steps 4a/4b (issue #693): the pass published its layout and registered the query
    // containers, so the settle loop can deliver the observations and re-style a container whose
    // condition crossed. The canvas has no `UpdateScheduler`, which the loop accepts as "no app
    // callbacks to flush".
    //
    // A relayout round calls back into `rebuild()`, so the loop must not be re-entered from there:
    // `in_settle_` makes the nested call stop after its own layout, and the OUTER loop continues
    // with the next round.
    if (!in_settle_) {
        in_settle_ = true;
        (void)settle_.run();
        in_settle_ = false;
    }
    // After layout, so a behavior that reads a node's box sees the final one. A behavior attaches
    // only when every target ref resolved to a built node.
    attach_behaviors(built);
    if (kept_focus != 0) {
        if (ui::Node* node = node_by_automation_id(kept_focus)) {
            ui::FocusManager& focus = router_.focus_manager();
            focus.set_root(*root_);
            if (focus.request_focus(node) && kept_ring) {
                focus.set_focus_visible(true);
            }
        }
    }
}

bool PreviewSession::relayout() {
    ui::layout::LayoutEngine engine(options_.measurer);
    const SceneLayoutResult laid_out =
        layout_scene(*root_, sheet_, engine, layout_metrics_, options_.measurer, &settle_);
    if (!laid_out.error.empty()) {
        return false;
    }
    metrics_ = laid_out.metrics;
    // The settle loop runs in the same call, so the first frame after a resize is already settled:
    // a container query that crossed a condition has re-styled and re-laid out before this returns
    // (issue #693's "no frame with the stale layout" criterion). A relayout round calls back into
    // `rebuild()`, which must not re-enter the loop, so the nested call is skipped.
    if (!in_settle_) {
        in_settle_ = true;
        (void)settle_.run();
        in_settle_ = false;
    }
    // A frame is owed even in the case where every box happens to land where it already was: the
    // viewport the picture is painted into changed, and `needs_frame()` is the caller's only
    // signal.
    root_->mark_dirty(ui::Dirty::Layout | ui::Dirty::Paint | ui::Dirty::Render);
    return true;
}

bool PreviewSession::set_posture(hal::FoldPosture posture) {
    return relayout_at(layout_metrics_.width, layout_metrics_.height, layout_metrics_.dpi_scale,
                       posture, layout_metrics_.insets, layout_metrics_.cutouts);
}

bool PreviewSession::set_viewport(float width, float height, float dpi_scale) {
    return relayout_at(width, height, dpi_scale, layout_metrics_.posture, layout_metrics_.insets,
                       layout_metrics_.cutouts);
}

bool PreviewSession::relayout_at(float width, float height, float dpi_scale,
                                 hal::FoldPosture posture) {
    return relayout_at(width, height, dpi_scale, posture, layout_metrics_.insets,
                       layout_metrics_.cutouts);
}

bool PreviewSession::relayout_at(float width, float height, float dpi_scale,
                                 hal::FoldPosture posture, const hal::WindowInsets& insets,
                                 const hal::CutoutInfo& cutouts) {
    if (!built_) {
        return false;
    }
    SceneMetrics next = layout_metrics_;
    next.width = width;
    next.height = height;
    next.dpi_scale = dpi_scale > 0.0f ? dpi_scale : 1.0f;
    next.posture = posture;
    next.insets = insets;
    next.cutouts = cutouts;
    if (next == layout_metrics_) {
        return false; // every layout input is already in force: no pass, and no frame owed
    }

    // The media context is the viewport, so the move can change what `@media` answers (issue #436).
    // It is re-resolved only when it actually can: the boolean comparison below is the same
    // `evaluate(list, context)` the cascade's resolve step makes, so a size that crosses no
    // breakpoint leaves `sheet_` - and the whole paint cache behind it - untouched.
    const ui::style::MediaContext media =
        preview_media_context(next.width, next.height, next.dpi_scale);
    const bool restyle =
        options_.prepared.has_value() && options_.prepared->media_answer_can_change(media_, media);
    const SceneMetrics previous = layout_metrics_;
    const ui::style::MediaContext previous_media = media_;

    layout_metrics_ = next;
    if (restyle) {
        sheet_ = options_.prepared->resolve(media);
    }
    media_ = media;
    if (!relayout()) {
        // The tree is left describing the boxes it actually has, so a caller cannot report a
        // viewport that was never applied. The re-resolve is undone with it.
        layout_metrics_ = previous;
        media_ = previous_media;
        if (restyle) {
            sheet_ = options_.prepared->resolve(previous_media);
        }
        return false;
    }
    if (restyle) {
        ++cascade_generation_;
    }
    return true;
}
void PreviewSession::run_handler(const std::string& handler_id,
                                 const std::vector<ir::Action>* actions) {
    if (actions == nullptr) {
        unrun_.push_back(handler_id);
        return;
    }
    if (actions->empty()) {
        ran_.push_back(handler_id);
        return;
    }
    if (!evaluator_.run(*actions, scope_).has_value()) {
        unrun_.push_back(handler_id);
        return;
    }
    ran_.push_back(handler_id);
    dirty_ = true;
}

bool PreviewSession::dispatch(const PreviewInput& input) {
    ran_.clear();
    unrun_.clear();
    dirty_ = false;
    for (const auto& slot : slots_) {
        slot->reached = false;
    }
    const ui::InputEvent event = to_input_event(input, next_timestamp_s_);
    next_timestamp_s_ += kEventStepSeconds;
    const ui::InputDispatchResult result = router_.dispatch(*root_, event);
    for (const auto& slot : slots_) {
        if (!slot->reached) {
            continue;
        }
        run_handler(slot->handler_id, slot->can_run ? slot->actions : nullptr);
    }
    if (dirty_) {
        // The scope changed, so the tree's text and boxes are stale. Rebuilding is the preview's
        // `apply_state`: the scope is the source of truth and the scene is derived from it.
        // `rebuild()` re-attaches the behaviors to the new tree.
        rebuild();
    } else {
        // The component's own options did not change, but the SCENE did: a pointer moved, a press
        // began, focus moved. A behavior whose outputs are a function of interaction state has no
        // options, so the option hash would skip it forever; this is the trigger that makes a
        // Button look pressed in the preview.
        refresh_behaviors();
    }
    return result.dispatched;
}

PreviewClickResult PreviewSession::click(std::uint64_t automation_id) {
    PreviewClickResult result;
    ui::Node* const node = node_by_automation_id(automation_id);
    if (node == nullptr) {
        return result;
    }
    result.found = true;
    result.target_id = node->automation_id();
    const ui::rhi::Rect bounds = node->world_bounds();
    const float x = bounds.x + bounds.width * 0.5f;
    const float y = bounds.y + bounds.height * 0.5f;

    // The gesture a real pointer makes, through the router: move, press, release. Click synthesis
    // then picks the nearest common ancestor, so clicking a label reaches its button.
    bool dispatched = false;
    for (const PreviewInput::Kind kind :
         {PreviewInput::Kind::PointerMove, PreviewInput::Kind::PointerDown,
          PreviewInput::Kind::PointerUp}) {
        PreviewInput event;
        event.kind = kind;
        event.x = x;
        event.y = y;
        dispatched = dispatch(event) || dispatched;
    }
    result.dispatched = dispatched;
    result.ran = ran_;
    result.unrun = unrun_;
    return result;
}

ui::StateSnapshot PreviewSession::state() const {
    // The same cells `snapshot_state()` emits: declaration order, one `ui::StateCell` per cell.
    // Produced from the evaluator's scope rather than a generated field, but it is the identical
    // representation, so `parity::run_parity` can compare it directly.
    ui::StateSnapshot snapshot;
    for (const ir::State& cell : component_.state) {
        append_cell(snapshot, cell.id, cell.type);
    }
    for (const ir::AnimatedValue& cell : component_.animated_values) {
        append_cell(snapshot, cell.id, cell.type);
    }
    return snapshot;
}

void PreviewSession::append_cell(ui::StateSnapshot& out, const std::string& id,
                                 const std::string& type) const {
    if (!carryable_type(type)) {
        return; // a record cell has no StateCell form, matching emit_snapshot.cpp
    }
    const auto found = scope_.find(id);
    if (found == scope_.end()) {
        return;
    }
    const eval::Value& value = found->second;
    if (const eval::List* items = value.as_list()) {
        // A list cell keeps its elements; the declared element type selects the C++ vector the
        // generated snapshot would build.
        if (type == "list<bool>") {
            std::vector<bool> out_items;
            out_items.reserve(items->size());
            for (const eval::Value& item : *items) {
                if (const bool* flag = item.as_bool()) {
                    out_items.push_back(*flag);
                }
            }
            out.push_back(ui::state_cell(id, out_items));
        } else if (type == "list<int>") {
            std::vector<int> out_items;
            out_items.reserve(items->size());
            for (const eval::Value& item : *items) {
                if (const std::int32_t* whole = item.as_int()) {
                    out_items.push_back(static_cast<int>(*whole));
                }
            }
            out.push_back(ui::state_cell(id, out_items));
        } else {
            std::vector<double> out_items;
            out_items.reserve(items->size());
            for (const eval::Value& item : *items) {
                out_items.push_back(item.number());
            }
            out.push_back(ui::state_cell(id, out_items));
        }
        return;
    }
    if (const bool* flag = value.as_bool()) {
        out.push_back(ui::state_cell(id, *flag));
    } else if (const std::int32_t* whole = value.as_int()) {
        out.push_back(ui::state_cell(id, static_cast<int>(*whole)));
    } else if (const double* number = value.as_float()) {
        out.push_back(ui::state_cell(id, *number));
    } else if (const std::string* text = value.as_string()) {
        out.push_back(ui::state_cell(id, *text));
    }
}

CarriedState PreviewSession::capture() const {
    return capture_state(component_, scope_);
}

} // namespace tessera::compiler::codegen::cli
