// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/source_map.hpp"

#include <cstddef>
#include <string>

namespace tessera::compiler::rules {

// One `useResource(service.method, key, { suspend })` call in its accepted form (ADR-067 item 1,
// docs/specs/pending-ui-v1.md section 3.1). The analyzer records what the call wrote; lowering
// turns it into an IR `resources` entry. The service and method are resolved against the project's
// services manifest by `check_resource_services`, after `check_services` declared the bindings.
struct ResourceInfo {
    std::size_t statement = 0; // index into the component body
    // The local name the result was bound to (`feed`), or empty for a bare statement, which
    // lowering names `res::<method>_<n>` (the rule of `beh::` in behavior-hooks-v1.md section 5).
    std::string name;
    std::string service_binding; // the `useService` binding the method is read from (`api`)
    std::string method;          // `loadFeed`
    bool suspend = false;
    const jsx::Expr* key = nullptr; // a scalar expression or an object literal of scalars
    // The IR type of `.data`: the manifest method's return type when it is int, float, bool or
    // string, empty otherwise (a record or `void` result has no scalar form in V1).
    std::string type;
    jsx::Span span;
};

} // namespace tessera::compiler::rules
