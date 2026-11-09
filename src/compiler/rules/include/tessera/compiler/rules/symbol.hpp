// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/source_map.hpp"

#include <cstddef>
#include <string>

namespace tessera::compiler::rules {

enum class SymbolKind {
    Prop,          // a destructured prop, `{ initial = 0 }: P`
    PropsParam,    // the whole props parameter, `props: P`
    State,         // `x` of `const [x, setX] = useState(...)`
    StateSetter,   // `setX` of the same statement
    Ref,           // `const r = useRef<E>(null)`
    AnimatedValue, // `const v = useAnimatedValue(init)`
    Local,         // `const x = <pure expression>`
    Handler,       // `const f = (...) => ...`
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
