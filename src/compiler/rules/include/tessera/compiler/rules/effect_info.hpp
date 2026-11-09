// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tessera::compiler::rules {

// One `useEffect` statement after the form and dependency checks.
struct EffectInfo {
    std::size_t statement = 0;            // index into the component body
    const jsx::Expr* body = nullptr;      // the effect arrow
    const jsx::Expr* cleanup = nullptr;   // the cleanup arrow, or null
    std::string bind_id;                  // from `// Note: bind::id`, or empty
    const jsx::Expr* dependency_array = nullptr; // second argument of the call, or null
    std::vector<std::string> dependencies; // names as written, in order
    std::vector<std::string> reads;       // required dependency names, first read first
};

} // namespace tessera::compiler::rules
