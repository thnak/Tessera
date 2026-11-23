// SPDX-License-Identifier: MIT

#pragma once

#include "reporter.hpp"

#include <string>
#include <vector>

namespace tessera::compiler::rules {

// The reactive-name resolution shared by the effect rule (`effect_deps.cpp`) and the memo/callback
// rule (`memo_deps.cpp`, issue #829). Both compare a body's inferred reactive inputs with a
// declared list, so a memo over another memo resolves through to props and state the same way an
// effect's read of a local does (locals are inlined, decision 8).

// True for a symbol that is a dependency by itself: a prop, a state variable or an animated value.
// Setters, refs, handlers, locals and the props object are not.
[[nodiscard]] bool is_dependency_kind(SymbolKind kind);

// Adds what a read of `name` requires to `required`, inlining locals. `visiting` stops a local that
// mentions itself and must be empty at the top-level call.
void add_required(const Context& ctx, const std::string& name, std::vector<std::string>& required,
                  std::vector<std::string>& visiting);

// The reactive names `body` reads, first read first, with locals inlined. This is the inferred set
// both dependency rules compare a declared list against.
[[nodiscard]] std::vector<std::string> required_names(const Context& ctx, const jsx::Expr& body);

} // namespace tessera::compiler::rules
