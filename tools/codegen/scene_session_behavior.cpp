// SPDX-License-Identifier: MIT

// The preview session's behavior wiring (ADR-064 item 7, spike #658; behavior-hooks-v1.md section
// 10), split out of `scene_session.cpp` when the wiring pushed that file past the source-size
// limit.
//
// **Why it is its own unit.** Everything here is the one thing the preview does that the rest of
// the session does not: it builds the SAME `behavior::Instance` the generated base builds, from the
// same IR entry and the same registry, and drives it with the session's own normalized input.
// Keeping it together makes "the preview and the generated base agree about what a behavior is" a
// property of one file rather than a coincidence across two.

#include "scene_build.hpp"
#include "scene_session.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

// One evaluated option value, from the evaluator's own `Value`. A callback arg is handled by the
// caller (the preview does not resolve handler ids to thunks yet); this covers the three value
// kinds the manifest declares.
ui::behavior::ArgValue to_arg_value(const eval::Value& resolved) {
    ui::behavior::ArgValue value;
    if (const bool* boolean = resolved.as_bool()) {
        value.kind = ui::behavior::ArgValue::Kind::Bool;
        value.boolean = *boolean;
    } else if (const double* number = resolved.as_float()) {
        value.kind = ui::behavior::ArgValue::Kind::Number;
        value.number = *number;
    } else if (const std::int32_t* integer = resolved.as_int()) {
        // An IR `int` is a number option: the manifest declares `number`, and a behavior reads
        // `double`.
        value.kind = ui::behavior::ArgValue::Kind::Number;
        value.number = static_cast<double>(*integer);
    } else if (const std::string* text = resolved.as_string()) {
        value.kind = ui::behavior::ArgValue::Kind::String;
        // The string lives in the evaluator's own storage for a literal, or in the scope for a
        // state cell; either way it outlives this call, so the view is safe to keep.
        value.text = *text;
    }
    return value;
}

} // namespace

void PreviewSession::attach_behaviors(const SceneBuildResult& built) {
    behaviors_.clear();
    behavior_errors_.clear();
    if (component_.behaviors.empty()) {
        return;
    }
    // The ref -> scene node table the build walk filled. A ref that did not resolve (a conditional
    // branch that did not take, an empty list) has no entry, so its target stays 0 and the behavior
    // attaches nothing -- the same rule the generated base follows.
    for (const ir::Behavior& behavior : component_.behaviors) {
        BehaviorSlot slot;
        slot.name = behavior.behavior;
        for (const std::string& ref_id : behavior.targets) {
            std::uint64_t node_id = 0;
            for (const SceneBuildResult::SceneRefTarget& entry : built.ref_targets) {
                if (entry.ref == ref_id) {
                    node_id = entry.node_id;
                    break;
                }
            }
            slot.target_nodes.push_back(node_id);
        }
        // The evaluated value options, in manifest order. A callback arg carries its handler id; a
        // value arg is evaluated once here, because a behavior's options are not re-evaluated per
        // pointer move.
        for (const ir::BehaviorArg& arg : behavior.args) {
            if (!arg.handler.empty()) {
                ui::behavior::ArgValue value;
                value.kind = ui::behavior::ArgValue::Kind::Handler;
                value.handler = 0; // the preview does not resolve handler ids to thunks yet
                slot.args.push_back(value);
                continue;
            }
            const std::expected<eval::Value, eval::EvalError> evaluated =
                evaluator_.evaluate(arg.expr, scope_);
            if (!evaluated.has_value()) {
                // An option the evaluator cannot resolve leaves the arg at its default rather than
                // failing the whole build: a behavior with a defaulted option is still useful.
                slot.args.push_back(ui::behavior::ArgValue{});
                continue;
            }
            slot.args.push_back(to_arg_value(*evaluated));
        }
        behaviors_.push_back(std::move(slot));
    }
    // Attach after the whole list exists, so a target lookup can see every node.
    for (BehaviorSlot& slot : behaviors_) {
        std::vector<ui::Node*> nodes;
        nodes.reserve(slot.target_nodes.size());
        for (const std::uint64_t node_id : slot.target_nodes) {
            nodes.push_back(node_id == 0 ? nullptr : node_by_id(node_id));
        }
        const ui::behavior::Targets targets{nodes};
        const ui::behavior::ArgsView args{slot.args};
        if (!slot.instance.attach(slot.name, targets, ui::behavior::Services{&router_}, args)) {
            behavior_errors_.push_back("behavior `" + slot.name +
                                       "` did not attach: its targets are not all present, or the "
                                       "registry has no such behavior");
        }
    }
}

void PreviewSession::refresh_behaviors() {
    for (BehaviorSlot& slot : behaviors_) {
        if (!slot.instance.attached()) {
            continue;
        }
        slot.instance.refresh(ui::behavior::ArgsView{slot.args});
    }
}

void PreviewSession::detach_behaviors() {
    for (BehaviorSlot& slot : behaviors_) {
        slot.instance.detach();
    }
}

std::span<const ui::behavior::OutputValue>
PreviewSession::behavior_outputs(std::size_t index) const {
    if (index >= behaviors_.size()) {
        return {};
    }
    return behaviors_[index].instance.outputs();
}

std::string_view PreviewSession::behavior_name(std::size_t index) const {
    if (index >= behaviors_.size()) {
        return {};
    }
    return behaviors_[index].name;
}

bool PreviewSession::behavior_attached(std::size_t index) const {
    return index < behaviors_.size() && behaviors_[index].instance.attached();
}

} // namespace tessera::compiler::codegen::cli
