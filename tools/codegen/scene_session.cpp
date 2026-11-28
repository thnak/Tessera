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
    case PreviewInput::Kind::Text:
    case PreviewInput::Kind::CompositionStart:
    case PreviewInput::Kind::CompositionUpdate:
    case PreviewInput::Kind::CompositionEnd:
        fill_text_event(input, event);
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

namespace {

SceneMetrics metrics_of(const PreviewSession::Options& options) {
    SceneMetrics metrics;
    metrics.width = options.width;
    metrics.height = options.height;
    metrics.dpi_scale = options.dpi_scale;
    metrics.posture = options.posture;
    metrics.insets = options.insets;
    metrics.cutouts = options.cutouts;
    return metrics;
}

ui::style::CompiledStyleSheet initial_sheet(const PreviewSession::Options& options,
                                            const ui::style::CompiledStyleSheet& fallback) {
    if (!options.prepared.has_value()) {
        return fallback;
    }
    const ui::style::StyleEnvironment environment = preview_environment(metrics_of(options));
    return options.prepared->resolve(options.media, &environment);
}

} // namespace

PreviewSession::PreviewSession(const ir::IrModule& module, const ir::Component& component,
                               const ui::style::CompiledStyleSheet& sheet, Options options)
    : module_(module),
      component_(component),
      // A session given the media-independent form resolves it itself against the viewport it was
      // asked for (issue #436), so a caller cannot hand it a sheet compiled for a different
      // rectangle. A caller that passes a compiled sheet keeps it, which is every pre-#436 caller.
      sheet_(initial_sheet(options, sheet)),
      options_(std::move(options)),
      media_(options_.media),
      style_environment_(preview_environment(metrics_of(options_))) {
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
    // The size class the first frame starts at (issue #692): a component that reads
    // `useBreakpoint()` renders the class the viewport actually has, not the field's initial value.
    pushed_size_class_ = ui::responsive::size_class_for(media_);
    if (options_.carried != nullptr) {
        const std::vector<std::string> only_this{component.component_id};
        carry_report_ = apply_state(
            component, *options_.carried,
            options_.live_component_ids != nullptr ? *options_.live_component_ids : only_this,
            scope_);
    }
    if (!component_.resources.empty()) {
        pending_ = std::make_unique<PendingPreview>(component_);
        evaluator_.set_pending_session(&pending_->session());
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
    detach_text_fields();
    behaviors_.clear();
    behavior_errors_.clear();
    root_ = std::make_unique<ui::Node>(1);
    // The old tree is gone, so what was applied to it is gone too; the layer is re-applied below.
    applied_overrides_.clear();
    override_text_.clear();

    // Like the generated `pending_sync()`: observe before the walk picks each boundary's subtree.
    if (pending_) {
        pending_->sync();
    }
    SceneBuildResult built = build_scene(component_, *root_, scope_, &module_, &instance_scopes_,
                                         pending_ ? &pending_->session() : nullptr);
    // The new tree borrows the strings and scopes of this build; the old tree is already gone, so
    // the old strings can go with it.
    frames_ = built.frames;
    metadata_strings_ = std::move(built.strings);
    built_ = built.error.empty();
    // The environment values the build seeded with their initial are overwritten with the live
    // viewport's, so a component that reads `useBreakpoint()` renders the class the canvas actually
    // has rather than the field's initial value (issue #692).
    for (const ir::Environment& env : component_.environment) {
        if (env.provider == "breakpoint") {
            scope_[env.id] = eval::Value::of_string(std::string(
                ui::responsive::size_class_name(ui::responsive::size_class_for(media_))));
        }
    }
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
        bind_handler(handler.automation_id, handler.event, handler.handler, handler.frame);
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
    attach_text_fields(built);
    if (kept_focus != 0) {
        if (ui::Node* node = node_by_automation_id(kept_focus)) {
            ui::FocusManager& focus = router_.focus_manager();
            focus.set_root(*root_);
            if (focus.request_focus(node) && kept_ring) {
                focus.set_focus_visible(true);
            }
        }
    }
    sync_text_fields(); // after focus is restored: the caret shows only in the focused field
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

void PreviewSession::push_size_class() {
    // The class comes from the media context, not from `layout_metrics_.width` directly: the
    // context is what the stylesheet's `@media` rules resolve against, so the hook and a breakpoint
    // rule cannot disagree about which side of 600 px the viewport is on (issue #692).
    const ui::responsive::SizeClass next = ui::responsive::size_class_for(media_);
    if (next == pushed_size_class_) {
        return; // a resize inside one class costs nothing: no walk, no setter, no re-render
    }
    pushed_size_class_ = next;
    (void)environment_.push("breakpoint", ui::responsive::size_class_name(next));
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
        preview_media_context(next.width, next.height, next.dpi_scale, next.posture);
    // The environment (ADR-074) moves with the safe area and the posture the same way: a sheet is
    // re-resolved only when a variable it reads has a new value.
    const ui::style::StyleEnvironment environment = preview_environment(next);
    const bool restyle =
        options_.prepared.has_value() &&
        (options_.prepared->media_answer_can_change(media_, media) ||
         options_.prepared->environment_answer_can_change(style_environment_, environment));
    const SceneMetrics previous = layout_metrics_;
    const ui::style::MediaContext previous_media = media_;
    const ui::style::StyleEnvironment previous_environment = style_environment_;

    layout_metrics_ = next;
    if (restyle) {
        sheet_ = options_.prepared->resolve(media, &environment);
    }
    media_ = media;
    style_environment_ = environment;
    // `useBreakpoint()` reads the same media context the `@media` rules do (issue #692), so the
    // class is pushed here, before the pass that will paint it. A resize inside one class pushes
    // nothing, exactly as in `AppHost`.
    push_size_class();
    if (!relayout()) {
        // The tree is left describing the boxes it actually has, so a caller cannot report a
        // viewport that was never applied. The re-resolve is undone with it.
        layout_metrics_ = previous;
        media_ = previous_media;
        style_environment_ = previous_environment;
        if (restyle) {
            sheet_ = options_.prepared->resolve(previous_media, &previous_environment);
        }
        return false;
    }
    if (restyle) {
        ++cascade_generation_;
    }
    return true;
}
void PreviewSession::run_handler(const std::string& handler_id,
                                 const std::vector<ir::Action>* actions, std::size_t frame) {
    if (actions == nullptr) {
        unrun_.push_back(handler_id);
        return;
    }
    if (actions->empty()) {
        ran_.push_back(handler_id);
        return;
    }
    if (!run_actions(frame, *actions)) {
        unrun_.push_back(handler_id);
        return;
    }
    ran_.push_back(handler_id);
    dirty_ = true;
    if (pending_) {
        pending_->after_handler();
    }
}

bool PreviewSession::dispatch(const PreviewInput& input) {
    ran_.clear();
    unrun_.clear();
    dirty_ = false;
    text_consumed_ = false;
    for (const auto& slot : slots_) {
        slot->reached = false;
    }
    const ui::InputEvent event = to_input_event(input, next_timestamp_s_);
    next_timestamp_s_ += kEventStepSeconds;
    if (input.kind == PreviewInput::Kind::PointerMove ||
        input.kind == PreviewInput::Kind::PointerDown ||
        input.kind == PreviewInput::Kind::PointerUp) {
        has_pointer_ = true;
        pointer_x_ = input.x;
        pointer_y_ = input.y;
    } else if (input.kind == PreviewInput::Kind::PointerCancel) {
        has_pointer_ = false;
    }
    const ui::InputDispatchResult result = router_.dispatch(*root_, event);
    for (const auto& slot : slots_) {
        if (!slot->reached) {
            continue;
        }
        run_handler(slot->handler_id, slot->can_run ? slot->actions : nullptr, slot->frame);
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
    settle_behavior_outputs();
    sync_text_fields(); // focus may have moved: show or hide the caret
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

} // namespace tessera::compiler::codegen::cli
