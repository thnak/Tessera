// SPDX-License-Identifier: MIT

#pragma once

#include "ir_samples.hpp"
#include "ir_test_util.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <functional>
#include <string>
#include <utility>

namespace ir_test {

inline std::string codes_of(const ir::IrModule& module) {
    std::string out;
    for (const auto& diagnostic : ir::validate(module)) {
        out += diagnostic.code + ";";
    }
    return out;
}

// Applies `change` to a fresh valid module and expects exactly the listed codes, in order.
inline void expect(const std::string& what, const std::function<void(ir::IrModule&)>& change,
                   const std::string& expected) {
    ir::IrModule module = make_valid_module();
    change(module);
    check_eq(codes_of(module), expected, what);
}

inline ir::Component& list(ir::IrModule& module) {
    return module.components[0];
}
inline ir::Node& row_node(ir::IrModule& module) {
    return list(module).root.children[0];
}
inline ir::Node& label_node(ir::IrModule& module) {
    return list(module).root.children[1].children[0];
}

inline constexpr const char* kDuplicate = "TESSERA_IR_DUPLICATE_ID;";
inline constexpr const char* kMissing = "TESSERA_IR_MISSING_ID;";
inline constexpr const char* kUnresolved = "TESSERA_IR_UNRESOLVED;";
inline constexpr const char* kMalformed = "TESSERA_IR_MALFORMED;";
inline constexpr const char* kUnsupported = "TESSERA_IR_UNSUPPORTED;";

} // namespace ir_test
