// SPDX-License-Identifier: MIT

// A user component instance in the preview scene build (issue #594). The callee is built under a
// host `View` (the shape generated code gives an instance, docs/design/compiler-node-state.md), in
// a scope of its own: its props are the caller's expressions evaluated in the caller's scope, its
// state cells live in the session's `InstanceScopes` under the instance's path (so they survive a
// rebuild), and a `Slot` splices the instance's children back in under the CALLER's scope
// (ir-v1 section 3.4: slot content belongs to the caller).
//
// The instance node's `events` are the caller's callbacks for this instance. They are recorded on
// the instance's frame, and the session runs the caller's handler when the callee's handler
// reaches a `callProp` for that name.

#include "scene_build_internal.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli::internal {
namespace {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;
namespace ui = tessera::ui;

// FNV-1a of the instance path, never zero: zero means "not inside an instance".
std::uint64_t path_salt(const std::string& path) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : path) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return hash | 1ULL;
}

} // namespace

Builder::Context Builder::capture() const {
    return {scope_, loop_depth_, component_refs_, salt_, instance_key_, frame_index_, loop_path_};
}

void Builder::apply(const Context& context) {
    scope_ = context.scope;
    loop_depth_ = context.loop_depth;
    component_refs_ = context.refs;
    salt_ = context.salt;
    instance_key_ = context.key;
    frame_index_ = context.frame;
    loop_path_ = context.loop_path;
}

bool Builder::build_instance(const ir::Node& node, ui::Node& host, std::size_t depth) {
    if (module_ == nullptr || instances_ == nullptr) {
        return unsupported("a user component instance (`" + node.id +
                           "`) needs the project's components, which this build was not given");
    }
    const ir::Component* callee = nullptr;
    for (const ir::Component& candidate : module_->components) {
        if (candidate.id == node.component || candidate.name == node.component) {
            callee = &candidate;
            break;
        }
    }
    if (callee == nullptr) {
        return unsupported("a user component instance (`" + node.id + "`) names `" +
                           node.component + "`, which is not in the project");
    }
    const std::string key = instance_key_ + "/" + node.id + loop_path_;
    // The callee's scope: a state cell the instance already has is kept (the rebuild after a
    // click), its props are re-evaluated every build, and any cell it lacks starts at its
    // initializer. A callback prop and the `children` slot carry no value.
    eval::Scope& callee_scope = (*instances_)[key];
    for (const ir::Prop& prop : callee->props) {
        if (prop.type == "children" || prop.type == "callback") {
            continue;
        }
        const auto given = node.props.find(prop.name);
        const eval::Scope empty;
        const bool from_caller = given != node.props.end() && given->second != nullptr;
        const ir::ExprPtr& source = from_caller ? given->second : prop.default_value;
        eval::Value value = eval::Value::null();
        if (source != nullptr) {
            auto evaluated = evaluator_.evaluate(*source, from_caller ? *scope_ : empty);
            if (!evaluated) {
                return fail("prop `" + prop.name + "` of `" + node.id +
                            "`: " + evaluated.error().code + ": " + evaluated.error().message);
            }
            value = std::move(*evaluated);
        }
        if ((prop.type == "float" || prop.type == "length") && value.as_int() != nullptr) {
            value = eval::Value::of_float(value.number());
        }
        callee_scope["prop::" + prop.name] = std::move(value);
    }
    if (!seed_component_scope(*callee, callee_scope, result_.error)) {
        return false;
    }

    SceneInstanceFrame frame;
    frame.key = key;
    frame.component = callee;
    frame.parent = frame_index_;
    frame.scope = &callee_scope;
    frame.callbacks.insert(node.events.begin(), node.events.end());
    result_.frames.push_back(std::move(frame));

    const CallerFrame caller{capture(), &node};
    frames_.push_back(caller);
    scope_ = &callee_scope;
    loop_depth_ = 0;
    component_refs_ = nullptr;
    instance_key_ = key;
    frame_index_ = result_.frames.size() - 1;
    salt_ = path_salt(key);
    loop_path_.clear();
    const bool built = build_node(callee->root, host, depth + 1);
    apply(caller.context);
    frames_.pop_back();
    return built;
}

bool Builder::build_slot(const ir::Node& slot, ui::Node& parent, std::size_t depth) {
    if (frames_.empty()) {
        return unsupported("a `Slot` position (`" + slot.id + "`) outside a component instance");
    }
    // The content belongs to the caller: it is built under the caller's scope, loop locals, frame
    // and id salt, then the callee's context is put back.
    const CallerFrame caller = frames_.back();
    const Context callee_context = capture();
    frames_.pop_back();
    apply(caller.context);
    const bool built = build_children(*caller.instance, parent, depth);
    frames_.push_back(caller);
    apply(callee_context);
    return built;
}

} // namespace tessera::compiler::codegen::cli::internal
