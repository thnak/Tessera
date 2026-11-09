// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/module.hpp"

#include <string>
#include <vector>

namespace tessera::compiler::ir {

// One hard error of docs/specs/ir-v1.md section 9. `span` is the nearest component, node,
// binding or effect that carries one; expressions have no span of their own.
struct IrDiagnostic {
    std::string code; // TESSERA_IR_*
    std::string message;
    SourceSpan span;
};

// Checks an IR module and returns every hard error in a fixed order (module, then components
// in order, then bindings). An empty result means the module is valid. It never throws and
// never modifies the IR. Covered and not covered checks are listed in
// docs/design/jsx-ir-lowering.md section 6.
//
// Codes: TESSERA_IR_VERSION, _DUPLICATE_ID, _MISSING_ID, _AUTOMATION_ID, _UNRESOLVED,
// _UNSUPPORTED, _MALFORMED, _SPAN, _ORDER, _LIST_KEY, _LOOP, _LEAF, _ERROR_BOUNDARY, _BINDING,
// _LITERAL.
std::vector<IrDiagnostic> validate(const IrModule& module);

} // namespace tessera::compiler::ir
