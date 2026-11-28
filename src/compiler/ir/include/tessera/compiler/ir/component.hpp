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

// One `case` of a reducer (ADR-066 item 4): the action's `type` literal and the handler the case
// became. `default` has an empty `action`.
struct DispatchCase {
    std::string action;  // the `type` literal (`"increment"`); empty for `default`
    std::string handler; // `handler::<N>`
};

struct State {
    std::string id;
    std::string name;
    std::string type;
    ExprPtr initial;
    // ADR-066 item 4: the dispatch table of a `useReducer` state, in `case` order. Empty for every
    // `useState`, which is what keeps each existing golden byte-identical. A `dispatch(...)` call
    // reads this rather than a second IR concept, so the reducer needs no new state kind: it is one
    // state cell plus one handler per case.
    std::vector<DispatchCase> dispatch{};
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
    // `useMergedRefs` (ADR-064 item 7, issue #630): the `ref::` ids this ref also assigns when its
    // target resolves. A merged ref is one assignment that fills several owners, so a component
    // that both forwards a ref and keeps an internal one does not need two `ref` attributes on one
    // node. Empty for an ordinary `useRef`.
    std::vector<std::string> aliases;
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
    // The one argument a parameterized environment hook takes (`useMediaQuery("(min-width:
    // 600px)")`), or an empty expression when the hook takes none. The generated base evaluates it
    // once and re-evaluates only when the component updates, so a query built from state follows
    // the state.
    Expr argument;
    SourceSpan source;
};

// One `const <name> = useFocusTrap(...)` behavior call (ADR-064 item 7;
// docs/specs/behavior-hooks-v1.md section 6). The generated base attaches, updates and detaches the
// engine instance; the IR carries everything it needs to do that without the manifest, which is why
// `args` lists every field in manifest order including the defaulted ones and `exclusive` is copied
// rather than looked up.
//
// Additive: a component without behaviors writes the bytes it wrote before, so `irVersion` stays 1
// and every committed IR golden is unchanged.
struct BehaviorArg {
    std::string name;
    // A value option: the expression the generated base evaluates and hashes. Empty when the arg is
    // a callback.
    Expr expr;
    // A callback option: the handler id (`handler::N` or `bind::<id>`). Empty when the arg is a
    // value.
    std::string handler;
};

struct BehaviorOutput {
    std::string name;
    std::string type; // the IR type of `ir-v1.md` section 2 (`bool`, `number`, `string`)
};

struct Behavior {
    std::string id;       // `beh::<name>`
    std::string hook;     // the TSX hook name (`useFocusTrap`)
    std::string behavior; // the engine registry name (`focus_trap`)
    // The `ref::` ids the behavior attaches to, in manifest order. A behavior is attached only when
    // every one holds a node.
    std::vector<std::string> targets;
    std::vector<BehaviorArg> args;
    std::vector<BehaviorOutput> outputs;
    // Copied from the manifest so the validator needs no manifest either.
    bool exclusive = false;
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
    // The manifest's reverse-DNS id (`demo.nav`), when the requirement came from a project
    // `ui/services.json` (docs/specs/services-manifest-v1.md section 3 item 5). Codegen emits a
    // `static_assert` that the C++ interface's `kServiceId` is `service_id(registry_id)`, so a
    // manifest that disagrees with the header is a compile error, not a runtime miss. Empty means
    // no check.
    std::string registry_id{};
};

// One `const <name> = useObservable(service.member)` read (ADR-050 open question 6,
// docs/design/observable-render-read.md). The member is a service-owned `Observable<T>` the
// project manifest lists; `obs::<name>` (and `obs::<name>.<field>` for a record) is the reference
// id that reads its current value. Codegen caches the observable's address at attach, subscribes
// the component's update at mount and unsubscribes at unmount, so a change re-renders the bindings
// that read it. Additive: a component without observables writes the bytes it wrote before.
struct Observable {
    std::string id;      // `obs::<name>`
    std::string service; // the `svc::` id of a declared service of this component
    std::string member;  // the manifest observable (the C++ accessor name)
    std::string type;    // the IR type of the value (`string`, `int`, a declared record, ...)
    SourceSpan source;
};

// One `const <name> = useResource(service.method, key, { suspend })` read (ADR-067 item 1;
// docs/specs/pending-ui-v1.md section 3.4). The resource is the typed, keyed record of
// async-data-v1.md section 1; its fetch goes through the declared service, never a URL.
//
// `key` is a scalar expression or a `record` of scalar expressions of state, props and environment
// values; two keys are the same key when they are equal by value, and a key change starts a new
// epoch. `res::<name>.status`, `.data` and `.error` are the reference ids that read it. Additive:
// a component without resources writes the bytes it wrote before.
struct Resource {
    std::string id;      // `res::<name>`
    std::string service; // the `svc::` id of a declared service of this component
    std::string method;  // the service method that fetches
    // A suspending read: the Suspense boundary above its `.data` reads shows its fallback while
    // the status is Loading with no data. False never suspends.
    bool suspend = false;
    // The IR type of `.data`, taken from the service method's declared return type when the
    // analyzer knows it (pending-ui-v1.md section 3.4, slice 3). Empty when it does not: then a
    // `.data` read validates but is not generated (TESSERA_CODEGEN_UNSUPPORTED).
    std::string type;
    Expr key;
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
    // ADR-064 item 7: the behavior calls, in declaration order. Empty for every component that
    // calls no behavior hook, which is what keeps the generated base (and every committed golden)
    // free of behavior code.
    std::vector<Behavior> behaviors;
    // ADR-050 section 3.1 (AF5): the component's declared service requirements, in source order.
    // Empty for every component that does not call `useService`, which is what keeps the generated
    // base (and every committed golden) free of service code.
    std::vector<Service> services;
    // ADR-067 item 1: the `useResource` reads, in declaration order. Empty for every component that
    // calls none, which keeps the IR JSON and the generated base byte-identical to before.
    std::vector<Resource> resources;
    // ADR-050 open question 6: the `useObservable` reads, in declaration order. Empty for every
    // component that calls none, which keeps the IR JSON and the generated base byte-identical.
    std::vector<Observable> observables;
    Node root;
};

} // namespace tessera::compiler::ir
