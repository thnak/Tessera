// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"

#include <cstddef>
#include <string>

namespace tessera::compiler::rules {

// One module-scope `const Ctx = createContext<T>(default);` after the form check (ADR-066 item 5,
// issue #802). The analyzer records what the const declared; lowering turns it into the IR
// environment the consumers name, and the codegen provider walk resolves each consumer against it.
struct ContextInfo {
    std::size_t item = 0; // index into Module::items
    // The const's name (`ThemeContext`), which the provider tag and the useContext argument both
    // spell.
    std::string name;
    // The declared value type (`Theme`), or empty for an inferred one (`createContext({ accent:
    // "blue" })` with no explicit argument). The declared form is the one V1 accepts; the inferred
    // one is a diagnostic, because the IR type has to be written somewhere.
    std::string type;
    // The IR type of the value: a declared record name, or a primitive. Empty when the type
    // argument does not resolve in this module, which is a diagnostic.
    std::string ir_type;
    // The `createContext` default, `{ accent: "blue" }`. The consumer with no ancestor provider
    // falls back to it (the owner's decision for #802), so lowering records it on the environment.
    const jsx::Expr* default_value = nullptr;
    jsx::Span span;
};

// One `const theme = useContext(ThemeContext);` call after the form check (ADR-066 item 5, issue
// #802). Lowering turns it into an IR environment entry whose provider names the context; the
// codegen provider walk decides, per instance, which ancestor provider's value it reads — or the
// context default when no ancestor provides one.
struct ContextUseInfo {
    std::size_t statement = 0; // index into the component body
    // The local name the value was bound to (`theme`). Empty for a bare statement, which is a form
    // error: an environment value nobody reads is dead code, and the IR has no id for it.
    std::string name;
    // The context const this call names (`ThemeContext`).
    std::string context;
    jsx::Span span;
};

} // namespace tessera::compiler::rules
