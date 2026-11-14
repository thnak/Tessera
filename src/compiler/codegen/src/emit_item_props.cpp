// SPDX-License-Identifier: MIT

// Props of a per-item component instance (docs/design/compiler-node-state.md section 3). The static
// path emits everything into the caller struct (`apply_props()`); an instance inside a list
// template lives in the keyed item record, so its props are emitted against that record instead:
// the initial values when the item is built, and the hash-guarded refresh the per-item content pass
// runs.

#include "emit_events.hpp"
#include "emit_props.hpp"
#include "value_ops.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

std::string_view list_element(std::string_view type) {
    if (type.size() > 6 && type.rfind("list<", 0) == 0 && type.back() == '>') {
        return type.substr(5, type.size() - 6);
    }
    return {};
}

bool is_string_type(std::string_view type) {
    return type == "string" || type == "color";
}

// The C++ text of one prop value, with the loop locals of the item template in scope. An array
// literal is emitted as a braced list (`emit_expr` rejects one outside an initializer position).
bool emit_value_text(std::string_view field, const PropBinding& binding, const ir::Component& owner,
                     const LocalBindings& locals, ExprEmission& out, std::string& error) {
    (void)field;
    if (binding.value->kind != ir::ExprKind::Array) {
        out = emit_expr(*binding.value, owner, locals);
        if (!out.error.empty()) {
            return fail(error, out.error);
        }
        return true;
    }
    std::string text = "{";
    for (const ir::ExprPtr& item : binding.value->operands) {
        if (!item) {
            return fail(error, "prop `" + binding.name + "` has an empty list element");
        }
        const ExprEmission element = emit_expr(*item, owner, locals);
        if (!element.error.empty()) {
            return fail(error, element.error);
        }
        if (text.size() > 1) {
            text += ", ";
        }
        text += element.text;
    }
    text += "}";
    out.text = std::move(text);
    out.error.clear();
    return true;
}

// The hash statements for one per-item value prop, when the value has an allocation-free hash form:
// a printable scalar, or a list read from a local reference. Returns false when the value cannot be
// hashed this way; the caller then assigns it unguarded.
bool emit_item_value_hash(CppWriter& writer, const PropBinding& binding, const std::string& value,
                          const std::string& hash_var) {
    const std::string_view element = list_element(binding.type);
    if (element.empty()) {
        if (is_string_type(binding.type)) {
            writer.line(hash_var + " = tessera::ui::effect_hash(" + hash_var +
                        ", std::string_view(" + value + "));");
        } else {
            writer.line(hash_var + " = tessera::ui::effect_hash(" + hash_var + ", " + value + ");");
        }
        return true;
    }
    if (binding.value->kind != ir::ExprKind::Ref) {
        return false;
    }
    writer.line(hash_var + " = tessera::ui::effect_hash(" + hash_var +
                ", static_cast<std::int64_t>(" + value + ".size()));");
    writer.open("for (const auto& tessera_item_value : " + value + ")");
    if (is_string_type(element)) {
        writer.line(hash_var + " = tessera::ui::effect_hash(" + hash_var +
                    ", std::string_view(tessera_item_value));");
    } else {
        writer.line(hash_var + " = tessera::ui::effect_hash(" + hash_var +
                    ", tessera_item_value);");
    }
    writer.close("");
    return true;
}

} // namespace

bool emit_item_instance_props(CppWriter& writer, const ir::Component& owner,
                              const ComponentInstance& instance, std::string_view record,
                              std::string_view field, const LocalBindings& locals, bool initial,
                              std::string& error) {
    for (const PropBinding& binding : instance.props) {
        const std::string member = std::string(field) + "->" + member_name("prop::" + binding.name);
        if (binding.is_callback()) {
            if (!initial) {
                continue; // the lambda is bound once, when the instance is built
            }
            if (binding.type == "callback") {
                writer.line(member + " = [this]() {");
                ++writer.indent;
                writer.line("this->" + binding.callback + "(tessera::ui::Event{});");
                --writer.indent;
                writer.line("};");
            } else {
                const PayloadRecord* payload = find_payload(binding.type);
                if (payload == nullptr) {
                    return fail(error,
                                "prop `" + binding.name + "` has an unsupported payload type");
                }
                writer.line(member + " = [this](const tessera::ui::" +
                            std::string(payload->cpp_type) + "& event) {");
                ++writer.indent;
                writer.line("this->" + binding.callback + "(event);");
                --writer.indent;
                writer.line("};");
            }
            continue;
        }
        ExprEmission value;
        if (!emit_value_text(field, binding, owner, locals, value, error)) {
            return false;
        }
        if (initial) {
            writer.line(member + " = " + value.text + ";");
            continue;
        }
        // Per-item refresh: an unchanged value copies and allocates nothing (the same FNV-1a
        // contract apply_state() uses). The guard lives in the item record, next to the instance.
        CppWriter guard;
        guard.indent = writer.indent;
        const std::string hash_var =
            "item_prop_" + std::to_string(instance.ordinal) + "_" + binding.name + "_hash";
        guard.line("std::uint64_t " + hash_var + " = tessera::ui::kEffectHashSeed;");
        if (prop_guard(binding.type) == PropGuard::Value &&
            emit_item_value_hash(guard, binding, value.text, hash_var)) {
            const std::string hash_member =
                std::string(record) + "." + prop_guard_member(instance.ordinal, binding.name);
            writer.text += guard.text;
            writer.line("if (" + hash_var + " != " + hash_member + ") {");
            ++writer.indent;
            writer.line(hash_member + " = " + hash_var + ";");
            writer.line(member + " = " + value.text + ";");
            --writer.indent;
            writer.line("}");
            continue;
        }
        writer.line(member + " = " + value.text + ";");
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
