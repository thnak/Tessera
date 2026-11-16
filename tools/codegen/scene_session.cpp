// SPDX-License-Identifier: MIT

#include "scene_build.hpp"
#include "scene_layout.hpp"
#include "scene_session.hpp"

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

// The scene event type an IR event name maps to, for the events this slice binds. A click bubbles
// through bindings (event.cpp), so binding Click is what a scripted click needs.
constexpr ui::EventType* click_type() {
    static ui::EventType type = ui::EventType::Click;
    return &type;
}

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
    event.modifiers = static_cast<ui::Modifier>(input.modifiers & 0x3FU);
    return event;
}

// A native binding with no preview body installs no actions. A static empty vector keeps the slot's
// "resolved, but nothing to run" state distinct from "not resolved at all", which is what decides
// whether a dispatch reports the handler as run or as unrun.
const std::vector<ir::Action> empty_actions{};

// One installed scene binding. `bind_event` takes a function pointer plus a `void*`, so the IR
// handler identity has to live in the context; this record is that context.
struct PreviewSession::HandlerSlot {
    std::string handler_id;
    const std::vector<ir::Param>* params = nullptr;
    const std::vector<ir::Action>* actions = nullptr;
    bool can_run = false;
    // Set by the scene callback: this dispatch reached the handler. Consumed (and cleared) by
    // `dispatch()`, which runs the body after the router has finished walking the tree.
    bool reached = false;
    std::string reason; // why not, when `can_run` is false
};

PreviewSession::PreviewSession(const ir::IrModule& module, const ir::Component& component,
                               const ui::style::CompiledStyleSheet& sheet, const Options& options)
    : module_(module),
      component_(component),
      sheet_(sheet),
      options_(options) {
    layout_metrics_.width = options_.width;
    layout_metrics_.height = options_.height;
    layout_metrics_.dpi_scale = options_.dpi_scale;
    layout_metrics_.posture = options_.posture;
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
    // The router holds raw capture/hover/active pointers into the tree about to be replaced and the
    // focus manager resolves ids under the old root, so both must drop that state before the old
    // tree dies (input_dispatch.hpp documents the same ordering for every caller).
    router_.reset();
    slots_.clear();
    root_ = std::make_unique<ui::Node>(1);

    const SceneBuildResult built = build_scene(component_, *root_, scope_);
    built_ = built.error.empty();
    if (!built_) {
        // A tree that cannot be built installs no handlers, so a page sees an empty snapshot and
        // reports the same unsupported construct the P2 preview reports.
        return;
    }
    for (const SceneBuildResult::SceneEventHandler& handler : built.handlers) {
        bind_handler(handler.automation_id, handler.event, handler.handler);
    }

    ui::layout::LayoutEngine engine(options_.measurer);
    const SceneLayoutResult laid_out =
        layout_scene(*root_, sheet_, engine, layout_metrics_, options_.measurer);
    metrics_ = laid_out.metrics;
}

bool PreviewSession::set_posture(hal::FoldPosture posture) {
    if (!built_ || layout_metrics_.posture == posture) {
        return false;
    }
    layout_metrics_.posture = posture;
    ui::layout::LayoutEngine engine(options_.measurer);
    const SceneLayoutResult laid_out =
        layout_scene(*root_, sheet_, engine, layout_metrics_, options_.measurer);
    if (!laid_out.error.empty()) {
        // The tree stays at the previous posture rather than half-applied: the error is a layout
        // failure, not a posture the caller asked for, and the metrics are left describing what
        // the boxes actually are.
        return false;
    }
    metrics_ = laid_out.metrics;
    return true;
}

void PreviewSession::bind_handler(std::uint64_t automation_id, const std::string& event,
                                  const std::string& handler_id) {
    auto slot = std::make_unique<HandlerSlot>();
    slot->handler_id = handler_id;

    // Resolve the IR body: a component handler (`handler::N`), or a native binding (`bind::X`)
    // whose `preview` block is the body. This mirrors `find_body` in
    // tests/parity/parity_runner.cpp, including the two cases it also refuses to run.
    for (const ir::Handler& handler : component_.handlers) {
        if (handler.id == handler_id) {
            slot->params = &handler.params;
            slot->actions = &handler.actions;
            break;
        }
    }
    if (slot->actions == nullptr) {
        for (const ir::Binding& binding : module_.bindings) {
            if (binding.id != handler_id ||
                (binding.component != component_.name && binding.component != component_.id)) {
                continue;
            }
            slot->params = &binding.params;
            // A native binding with no preview body does nothing in the preview, exactly as in the
            // parity runner: the developer's C++ is the thing this slice does not run.
            slot->actions = binding.preview ? &binding.preview->actions : &empty_actions;
            break;
        }
    }
    if (slot->actions == nullptr) {
        slot->reason = "no handler or native binding with that id in `" + component_.name + "`";
    } else if (slot->params != nullptr && !slot->params->empty()) {
        // A payload-taking handler needs a record value (`ClickEvent`, `KeyEvent`) bound to its
        // parameter, which the evaluator cannot construct: the preview dispatches the event but
        // cannot run this body. Reported, never silently treated as "nothing happened".
        slot->reason = "the handler takes a payload parameter (`" + slot->params->front().name +
                       "`), which the evaluator has no record value for";
    } else {
        slot->can_run = true;
    }

    HandlerSlot* const raw = slot.get();
    slots_.push_back(std::move(slot));

    if (event != "onClick") {
        return; // an IR event with no scene counterpart in this slice
    }
    if (ui::Node* node = node_by_automation_id(automation_id)) {
        (void)node->bind_event(*click_type(), &PreviewSession::on_scene_event, raw);
    }
}

void PreviewSession::on_scene_event(void* context, const ui::Event& event) {
    auto* const slot = static_cast<HandlerSlot*>(context);
    if (slot == nullptr || event.type != ui::EventType::Click) {
        return;
    }
    // The binding only reaches a handler that exists; the session runs it from the post-dispatch
    // pass, because a body that mutates state must not rebuild the tree it is being dispatched
    // through. Marking the slot is enough: the router visits ancestors in order, and `dispatch()`
    // runs every marked slot in binding order afterwards.
    slot->reached = true;
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
        rebuild();
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

ui::Node* PreviewSession::node_by_automation_id(std::uint64_t automation_id) {
    if (automation_id == 0) {
        return nullptr;
    }
    // Preorder search under the live tree, skipping hidden subtrees the way the harness's
    // `find_visible` does: a hidden node is not clickable, so a script cannot reach one.
    struct Walk {
        static ui::Node* find(ui::Node& node, std::uint64_t id) {
            if (!node.rendered()) {
                return nullptr;
            }
            if (node.automation_id() == id) {
                return &node;
            }
            for (const auto& child : node.children()) {
                if (ui::Node* found = find(*child, id)) {
                    return found;
                }
            }
            return nullptr;
        }
    };
    return Walk::find(*root_, automation_id);
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
