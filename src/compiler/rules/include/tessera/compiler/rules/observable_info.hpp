// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/source_map.hpp"

#include <cstddef>
#include <string>

namespace tessera::compiler::rules {

// One `const route = useObservable(nav.route);` statement in its accepted form (ADR-050 open
// question 6, docs/design/observable-render-read.md). `service_binding` is the `useService` binding
// the member is read from (`nav`) and `member` the manifest observable (`route`); `type` is the
// manifest's IR type of its value. Lowering turns it into an IR `observables` entry.
struct ObservableInfo {
    std::size_t statement = 0; // index into the component body
    std::string name;          // the local the value is bound to (`route`)
    std::string service_binding;
    std::string member;
    std::string type; // filled by `check_observable_services` from the manifest
    jsx::Span span;
};

} // namespace tessera::compiler::rules
