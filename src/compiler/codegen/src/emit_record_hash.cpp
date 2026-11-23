// SPDX-License-Identifier: MIT

// ADR-051 implementation note (issue #834): the generated field-wise FNV-1a hash of one record. The
// hash is the change-detection key the prop guards and the per-item content pass already used
// scalar `effect_hash` calls for, extended to a record: it reads each member in declaration order,
// recurses through nested records and `list`/`optional` members, and allocates nothing (a list's
// elements are hashed in place, no temporary string).

#include "emit_record_hash.hpp"
#include "emit_types_header.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::compiler::codegen::internal {
namespace {

std::string_view inner_of(std::string_view type, std::string_view wrapper) {
    if (type.size() > wrapper.size() + 1 && type.rfind(wrapper, 0) == 0 && type.back() == '>') {
        return type.substr(wrapper.size(), type.size() - wrapper.size() - 1);
    }
    return {};
}

bool scalar_hashable(std::string_view type) {
    return type == "bool" || type == "int" || type == "float" || type == "length";
}

bool string_like(std::string_view type) {
    return type == "string" || type == "color";
}

// Folds one value of `type` into the running `hash`. `access` is a C++ expression naming the value;
// `depth` keeps a distinct loop variable per nested list level. False with `error` set for a type
// that has no generated hash (`node_ref`, or an unknown name in a hand-built module).
bool emit_value_hash(CppWriter& writer, std::string_view type, const std::string& access,
                     const ir::IrModule& module, int depth, std::string& error) {
    if (const std::string_view element = inner_of(type, "list<"); !element.empty()) {
        // A list member: hash the length, then every element, so an unchanged list is
        // allocation-free and an empty list matches a fresh one.
        writer.line("hash = tessera::ui::effect_hash(hash, "
                    "static_cast<std::int64_t>(" + access + ".size()));");
        const std::string item = "tessera_record_item_" + std::to_string(depth);
        writer.open("for (const auto& " + item + " : " + access + ")");
        if (!emit_value_hash(writer, element, item, module, depth + 1, error)) {
            return false;
        }
        writer.close("");
        return true;
    }
    if (const std::string_view element = inner_of(type, "optional<"); !element.empty()) {
        // An optional member: presence is part of the value; the inner value follows when set.
        writer.line("hash = tessera::ui::effect_hash(hash, " + access + ".has_value());");
        writer.open("if (" + access + ".has_value())");
        if (!emit_value_hash(writer, element, "*" + access, module, depth, error)) {
            return false;
        }
        writer.close("");
        return true;
    }
    if (find_record_type(module, type) != nullptr) {
        // A nested record folds its own field-wise hash in as a 64-bit value.
        writer.line("hash = tessera::ui::effect_hash(hash, hash_value(" + access + "));");
        return true;
    }
    if (string_like(type)) {
        // A bare string would bind the string_view overload ambiguously with the char* one; name
        // it, as the scalar prop guards do.
        writer.line("hash = tessera::ui::effect_hash(hash, std::string_view(" + access + "));");
        return true;
    }
    if (scalar_hashable(type)) {
        writer.line("hash = tessera::ui::effect_hash(hash, " + access + ");");
        return true;
    }
    error = "TESSERA_CODEGEN_UNSUPPORTED: type `" + std::string(type) + "` has no generated hash";
    return false;
}

} // namespace

bool emit_record_hash_definition(CppWriter& writer, const ir::RecordType& record,
                                 const ir::IrModule& module, std::string& error) {
    writer.line("// Field-wise FNV-1a of `" + record.name + "` (no allocation).");
    writer.open("inline std::uint64_t hash_value(const " + record.name + "& value)");
    writer.line("std::uint64_t hash = tessera::ui::kEffectHashSeed;");
    for (const ir::RecordMember& member : record.members) {
        const std::string type =
            member.optional ? "optional<" + member.type + ">" : member.type;
        std::string member_error;
        if (!emit_value_hash(writer, type, "value." + member.name, module, 0, member_error)) {
            error = "TESSERA_CODEGEN_UNSUPPORTED: member `" + member.name + "` of record `" +
                    record.name + "` " + member_error.substr(member_error.find(':') + 2);
            return false;
        }
    }
    writer.line("return hash;");
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
