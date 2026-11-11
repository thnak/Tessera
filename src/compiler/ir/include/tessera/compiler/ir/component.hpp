// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/node.hpp"

#include <string>
#include <vector>

namespace tessera::compiler::ir {

// docs/specs/ir-v1.md section 2.
struct Prop {
    std::string name;
    std::string type;
    ExprPtr default_value;
    // The source `?` marker: the caller may omit the prop. Kept separate from the type because an
    // absent prop is not the same value as a nullable (`optional<T>`) one, and because a default
    // value is a later slice (docs/design/jsx-ir-lowering.md section 9).
    bool optional = false;
};

struct State {
    std::string id;
    std::string name;
    std::string type;
    ExprPtr initial;
};

struct Effect {
    std::string id;
    std::vector<std::string> deps;
    std::string handler;
    std::string cleanup; // empty when absent
    SourceSpan source;
};

struct Ref {
    std::string id;
    std::string name;
    std::string target; // node id
};

struct AnimatedValue {
    std::string id;
    std::string name;
    std::string type;
    ExprPtr initial;
};

struct Environment {
    std::string id;
    std::string name;
    std::string provider;
    std::string type;
    bool writable = false;
    SourceSpan source;
};

// Inline setter handler (section 2.1).
struct Handler {
    std::string id;
    std::vector<Param> params;
    std::vector<Action> actions;
    SourceSpan source;
};

struct Component {
    std::string id;
    std::string component_id;
    std::string name;
    SourceSpan source;
    std::vector<Prop> props;
    std::vector<State> state;
    std::vector<Handler> handlers;
    std::vector<Effect> effects;
    std::vector<Ref> refs;
    std::vector<AnimatedValue> animated_values;
    std::vector<Environment> environment;
    Node root;
};

} // namespace tessera::compiler::ir
