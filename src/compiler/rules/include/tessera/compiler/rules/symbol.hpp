// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/source_map.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace tessera::compiler::rules {

enum class SymbolKind : std::uint8_t {
    Prop,          // a destructured prop, `{ initial = 0 }: P`
    PropsParam,    // the whole props parameter, `props: P`
    State,         // `x` of `const [x, setX] = useState(...)`
    StateSetter,   // `setX` of the same statement
    Ref,           // `const r = useRef<E>(null)`
    AnimatedValue, // `const v = useAnimatedValue(init)`
    Environment,   // `const bp = useBreakpoint()` (an `environment` hook value)
    Local,         // `const x = <pure expression>`
    Handler,       // `const f = (...) => ...`
    // ADR-066 item 4: the `dispatch` of `const [state, dispatch] = useReducer(...)`. It is a
    // function value like a `StateSetter`, so a handler body of only `dispatch({...})` calls is an
    // inline handler rather than a native one. Kept distinct from `StateSetter` because its
    // argument is an action object, not the state's value.
    Dispatch,
    // ADR-050 section 3.1 (issue #177): `nav` of `const nav = useService<Navigator>();`. It is not
    // a value: the only thing a component does with it is call a manifest method, so it has no IR
    // expression and a call on it lowers to a `callService` action.
    Service,
    // ADR-067 item 4 (issue #656): the two names of `const [isPending, startTransition] =
    // useTransition();`. `isPending` is a read-only bool, the one output of the `transition`
    // behavior, so it resolves to `beh::<name>.isPending`. `startTransition` is a function value
    // whose one use is a call whose argument is a setter-call arrow, so a body made of such calls
    // is an inline handler like one made of setter calls.
    TransitionPending,
    TransitionStart,
    // ADR-067 item 1 (issue #656): `feed` of `const feed = useResource(...)`. Not a value on its
    // own: `feed.status`, `feed.data` and `feed.error` resolve to `res::feed.<member>`.
    Resource,
    // ADR-067 item 5 (issue #656): `d` of `const d = useDeferredValue(x)`, a read-only scalar, the
    // one output of the `deferred_value` behavior (`beh::d.value`).
    DeferredValue,
};

inline constexpr std::size_t kNoStatement = static_cast<std::size_t>(-1);

// One name declared in a component. `statement` indexes ModuleItem::body; props have no statement.
struct Symbol {
    std::string name;
    SymbolKind kind = SymbolKind::Local;
    jsx::Span span;                       // the declaring statement, or the prop binding
    std::size_t statement = kNoStatement; // index into the component body
};

} // namespace tessera::compiler::rules
