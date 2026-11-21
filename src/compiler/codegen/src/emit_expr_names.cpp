// SPDX-License-Identifier: MIT

// Naming and literal spelling shared by the expression and action emitters (issue #327: split out
// of `emit_expr.cpp`, which had grown past the 500-line hard limit).
//
// These turn an IR name into a C++ spelling - `state::count` into `state_count_`, `anim::fade` into
// its controller member - and spell IR literals as C++ source. Every declaration here already lives
// in `codegen_internal.hpp`, because the emitters, the props/vector writers and `emit_call_prop`
// all ask for them; this file is only where the definitions live.

#include "codegen_internal.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tessera::compiler::codegen::internal {
namespace {

// The reference namespaces that resolve to generated members. `state::` and `anim::` are shared
// with the action emitter through codegen_internal.hpp; the rest are only asked here.
constexpr std::string_view kState = kStatePrefix;
constexpr std::string_view kProp = "prop::";
constexpr std::string_view kRef = "ref::";
constexpr std::string_view kAnim = kAnimPrefix;
constexpr std::string_view kEnv = "env::";
// A behavior output (ADR-064 item 7, behavior-hooks-v1.md section 5): `beh::<name>.<field>` reads
// the generated accessor `<member><field>()`, so the dotted form is the only valid shape.
constexpr std::string_view kBeh = "beh::";

// Locale-independent number text. `std::to_chars` never consults the locale, so a generated `0.5`
// cannot become `0,5` on a host with a comma decimal separator.
std::string number_text(double value) {
    char buffer[64];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof buffer, value);
    std::string out(buffer, result.ptr);
    if (out.find_first_of(".eE") == std::string::npos) {
        out += ".0";
    }
    return out;
}

} // namespace

std::string escape_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out += kHex[static_cast<unsigned char>(c) & 0xF];
            } else {
                out += c;
            }
            break;
        }
    }
    out += '"';
    return out;
}

std::optional<std::string> member_prefix(std::string_view prefix) {
    if (prefix == kState) {
        return std::string("state_");
    }
    if (prefix == kProp) {
        return std::string("prop_");
    }
    if (prefix == kRef) {
        return std::string("ref_");
    }
    if (prefix == kAnim) {
        return std::string("anim_");
    }
    if (prefix == kEnv) {
        return std::string("env_");
    }
    if (prefix == kBeh) {
        return std::string("beh_");
    }
    return std::nullopt;
}

std::string literal_text(const ir::LiteralValue& literal) {
    if (const auto* flag = std::get_if<bool>(&literal)) {
        return *flag ? "true" : "false";
    }
    if (const auto* integer = std::get_if<std::int64_t>(&literal)) {
        return std::to_string(*integer);
    }
    if (const auto* number = std::get_if<double>(&literal)) {
        return number_text(*number);
    }
    if (const auto* text = std::get_if<std::string>(&literal)) {
        return escape_string(*text);
    }
    return "nullptr";
}

std::string animated_controller_name(std::string_view ir_id) {
    std::string name = member_name(ir_id);
    if (!name.empty() && name.back() == '_') {
        name.pop_back();
    }
    return name + "_scalar_";
}

std::string animated_on_complete_name(std::string_view ir_id) {
    std::string name = member_name(ir_id);
    if (!name.empty() && name.back() == '_') {
        name.pop_back();
    }
    return name + "_on_complete_";
}

bool animated_has_completion(const ir::Component& component, std::string_view ir_id) {
    for (const ir::Handler& handler : component.handlers) {
        for (const ir::Action& action : handler.actions) {
            if (action.kind == ir::ActionKind::AnimSet && action.target == ir_id &&
                !action.on_complete.empty()) {
                return true;
            }
        }
    }
    return false;
}

std::string member_name(std::string_view ir_id) {
    const std::size_t split = ir_id.find("::");
    if (split == std::string_view::npos) {
        return std::string(ir_id) + "_";
    }
    const std::string_view prefix = ir_id.substr(0, split);
    const std::string_view name = ir_id.substr(split + 2);
    const std::optional<std::string> mapped = member_prefix(prefix);
    return (mapped ? *mapped : std::string(prefix) + "_") + std::string(name) + "_";
}

std::optional<std::string> cpp_type(std::string_view ir_type,
                                    std::span<const ir::RecordType> records) {
    if (ir_type == "bool") {
        return std::string("bool");
    }
    if (ir_type == "int") {
        return std::string("int");
    }
    if (ir_type == "float") {
        return std::string("double");
    }
    if (ir_type == "string" || ir_type == "color") {
        return std::string("std::string");
    }
    if (ir_type == "length") {
        return std::string("double");
    }
    if (ir_type == "node_ref") {
        return std::string("tessera::ui::Node*");
    }
    for (const std::string_view wrapper : {"list<", "optional<"}) {
        if (ir_type.size() > wrapper.size() + 1 && ir_type.rfind(wrapper, 0) == 0 &&
            ir_type.back() == '>') {
            const std::optional<std::string> inner = cpp_type(
                ir_type.substr(wrapper.size(), ir_type.size() - wrapper.size() - 1), records);
            if (!inner) {
                return std::nullopt;
            }
            if (wrapper == "list<") {
                return "std::vector<" + *inner + ">";
            }
            return "std::optional<" + *inner + ">";
        }
    }
    // ADR-051: a declared record is a generated aggregate struct in `<module>_types.hpp`. Without
    // the table a record name has no C++ form, which is how a callback prop stays a non-value.
    for (const ir::RecordType& record : records) {
        if (record.name == ir_type) {
            return record.name;
        }
    }
    return std::nullopt;
}

} // namespace tessera::compiler::codegen::internal
