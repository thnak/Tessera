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
    // absent prop is not the same value as a nullable (`optional<T>`) one. `default_value` is the
    // literal (or flat literal list) from the props destructuring default
    // (docs/specs/jsx-subset-v1.md section 6); it is the value codegen initializes the field with.
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

// One `const <name> = useService<Interface>();` requirement of a component (ADR-050 section 3.1,
// AF5). The component's requirement set is exactly this list: codegen resolves each entry once, in
// the generated base's `attach_services()`, and `service<T>()` is a compile error for any other T.
//
// `type` and `header` are the C++ spelling of the interface, which is what the `ui/services.json`
// manifest supplies (ADR-050 section 3.1 item 1). The IR carries them as data because codegen reads
// the IR only; the project's manifest reader fills them in. This is deliberately NOT the same as
// `Environment`: an environment value is read through a provider every time, a service is a typed
// pointer cached at attach.
struct Service {
    std::string id;     // `svc::<name>`
    std::string name;   // the TSX binding name the accessor is named after (`nav`)
    std::string type;   // the C++ service interface, fully qualified (`demo::Navigator`)
    std::string header; // the header that declares `type` (`demo/services.hpp`)
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
    // ADR-050 section 3.1 (AF5): the component's declared service requirements, in source order.
    // Empty for every component that does not call `useService`, which is what keeps the generated
    // base (and every committed golden) free of service code.
    std::vector<Service> services;
    Node root;
};

} // namespace tessera::compiler::ir
