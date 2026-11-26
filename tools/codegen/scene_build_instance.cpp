// SPDX-License-Identifier: MIT

// A user component instance in the preview scene build (issue #594). The callee is built under a
// host `View` (the shape generated code gives an instance, docs/design/compiler-node-state.md), in
// a scope of its own: its props are the caller's expressions evaluated in the caller's scope, its
// state starts at the callee's initializers, and a `Slot` splices the instance's children back in
// under the CALLER's scope (ir-v1 section 3.4: slot content belongs to the caller).
//
// Not here yet: a callee's own event handlers and state changes. They need a scope per instance in
// the session, and a callback prop (`onPress`) needs to reach the caller's handler; until then a
// callee's events are not bound and its state stays at its initial value.

#include "scene_build_internal.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli::internal {
namespace {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;
namespace ui = tessera::ui;

} // namespace

void Builder::enter_frame(const CallerFrame& caller) {
    frames_.push_back(caller);
}

void Builder::leave_frame(const CallerFrame& caller) {
    scope_ = caller.scope;
    loop_depth_ = caller.loop_depth;
    component_refs_ = caller.refs;
    salt_ = caller.salt;
    frames_.pop_back();
}

bool Builder::build_instance(const ir::Node& node, ui::Node& host, std::size_t depth) {
    if (module_ == nullptr) {
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
    // The callee's scope: its props (the caller's expressions, evaluated in the caller's scope),
    // then its own cells at their initial values. A callback prop and the `children` slot carry no
    // value; the slot content is spliced where the callee places a `Slot`.
    eval::Scope callee_scope;
    for (const ir::Prop& prop : callee->props) {
        if (prop.type == "children" || prop.type == "callback") {
            continue;
        }
        const auto given = node.props.find(prop.name);
        const eval::Scope empty;
        const ir::ExprPtr& source = (given != node.props.end() && given->second != nullptr)
                                        ? given->second
                                        : prop.default_value;
        eval::Value value = eval::Value::null();
        if (source != nullptr) {
            auto evaluated =
                evaluator_.evaluate(*source, source == prop.default_value ? empty : *scope_);
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

    enter_frame({scope_, loop_depth_, component_refs_, salt_, &node});
    const CallerFrame caller = frames_.back();
    scope_ = &callee_scope;
    loop_depth_ = 0;
    component_refs_ = nullptr;
    salt_ = ++instances_;
    const bool ok = build_node(callee->root, host, depth + 1);
    leave_frame(caller);
    return ok;
}

bool Builder::build_slot(const ir::Node& slot, ui::Node& parent, std::size_t depth) {
    if (frames_.empty()) {
        return unsupported("a `Slot` position (`" + slot.id + "`) outside a component instance");
    }
    // The content belongs to the caller: it is built under the caller's scope, loop locals and
    // instance salt, then the callee's context is put back.
    const CallerFrame caller = frames_.back();
    const CallerFrame callee_state{scope_, loop_depth_, component_refs_, salt_, nullptr};
    frames_.pop_back();
    scope_ = caller.scope;
    loop_depth_ = caller.loop_depth;
    component_refs_ = caller.refs;
    salt_ = caller.salt;
    const bool ok = build_children(*caller.instance, parent, depth);
    frames_.push_back(caller);
    scope_ = callee_state.scope;
    loop_depth_ = callee_state.loop_depth;
    component_refs_ = callee_state.refs;
    salt_ = callee_state.salt;
    return ok;
}

} // namespace tessera::compiler::codegen::cli::internal
