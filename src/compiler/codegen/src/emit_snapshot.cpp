// SPDX-License-Identifier: MIT

#include "emit_snapshot.hpp"

#include <string_view>

namespace tessera::compiler::codegen::internal {
namespace {

// A scalar a StateScalar holds: the IR types whose C++ form is bool, int, double or std::string.
bool scalar_type(std::string_view ir_type) {
    return ir_type == "bool" || ir_type == "int" || ir_type == "float" || ir_type == "length" ||
           ir_type == "string" || ir_type == "color";
}

bool snapshot_type(std::string_view ir_type) {
    constexpr std::string_view kList = "list<";
    if (ir_type.size() > kList.size() + 1 && ir_type.rfind(kList, 0) == 0 &&
        ir_type.back() == '>') {
        return scalar_type(ir_type.substr(kList.size(), ir_type.size() - kList.size() - 1));
    }
    return scalar_type(ir_type);
}

} // namespace

void emit_snapshot_declaration(CppWriter& writer) {
    writer.line("// Every state and animated cell by IR id, in declaration order (tests, parity).");
    writer.line("[[nodiscard]] tessera::ui::StateSnapshot snapshot_state() const;");
}

void emit_snapshot_definition(CppWriter& writer, const ir::Component& component) {
    writer.open("tessera::ui::StateSnapshot " + component.name + "::snapshot_state() const");
    writer.line("tessera::ui::StateSnapshot snapshot;");
    for (const ir::State& state : component.state) {
        if (snapshot_type(state.type)) {
            writer.line("snapshot.push_back(tessera::ui::state_cell(\"" + state.id + "\", " +
                        member_name(state.id) + "));");
        }
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        if (snapshot_type(value.type)) {
            writer.line("snapshot.push_back(tessera::ui::state_cell(\"" + value.id + "\", " +
                        member_name(value.id) + "));");
        }
    }
    writer.line("return snapshot;");
    writer.close("");
    writer.line("");
}

} // namespace tessera::compiler::codegen::internal
