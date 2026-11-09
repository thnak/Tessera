// SPDX-License-Identifier: MIT

// Deterministic text dumps of types and modules, for tests (no spans).

#include "tessera/compiler/jsx/parser.hpp"

namespace tessera::compiler::jsx {
namespace {

void append_type(std::string& out, const TypeNode& type) {
    switch (type.kind) {
    case TypeKind::Name:
        out += type.text;
        if (!type.children.empty()) {
            out.push_back('<');
            for (std::size_t i = 0; i < type.children.size(); ++i) {
                out += i == 0 ? "" : ",";
                append_type(out, *type.children[i]);
            }
            out.push_back('>');
        }
        return;
    case TypeKind::StringLiteral:
        out += "\"" + type.text + "\"";
        return;
    case TypeKind::Array:
        append_type(out, *type.children[0]);
        out += "[]";
        return;
    case TypeKind::Union:
        for (std::size_t i = 0; i < type.children.size(); ++i) {
            out += i == 0 ? "" : "|";
            append_type(out, *type.children[i]);
        }
        return;
    case TypeKind::Object:
        out.push_back('{');
        for (std::size_t i = 0; i < type.members.size(); ++i) {
            out += (i == 0 ? "" : ",") + type.members[i].name;
            out += type.members[i].optional ? "?:" : ":";
            append_type(out, *type.members[i].type);
        }
        out.push_back('}');
        return;
    case TypeKind::Function:
        out.push_back('(');
        for (std::size_t i = 0; i < type.param_names.size(); ++i) {
            out += (i == 0 ? "" : ",") + type.param_names[i] + ":";
            append_type(out, *type.children[i]);
        }
        out += ")=>";
        append_type(out, *type.children.back());
        return;
    }
}

std::string type_text(const TypeNode& type) {
    std::string out;
    append_type(out, type);
    return out;
}

const char* kind_name(ItemKind kind) {
    switch (kind) {
    case ItemKind::Import:
        return "import";
    case ItemKind::TypeAlias:
        return "type";
    case ItemKind::Const:
        return "const";
    case ItemKind::Component:
        return "component";
    case ItemKind::Unknown:
        break;
    }
    return "unknown";
}

void dump_statement(std::string& out, const Statement& statement) {
    if (statement.kind == StatementKind::Expression) {
        out += " (expr " + dump(*statement.init) + ")";
        return;
    }
    out += " (const ";
    out += statement.array_pattern ? "[" : "";
    for (std::size_t i = 0; i < statement.names.size(); ++i) {
        out += (i == 0 ? "" : " ") + statement.names[i];
    }
    out += statement.array_pattern ? "]" : "";
    if (statement.type) {
        out += " : " + type_text(*statement.type);
    }
    out += " " + dump(*statement.init) + ")";
}

void dump_item(std::string& out, const ModuleItem& item) {
    out += "\n  ";
    if (!item.valid) {
        out += std::string("(invalid ") + kind_name(item.kind) +
               (item.name.empty() ? "" : " " + item.name) + ")";
        return;
    }
    switch (item.kind) {
    case ItemKind::Import:
        out += item.side_effect ? "(import-side-effect \"" + item.source + "\")"
                                : "(import \"" + item.source + "\"";
        for (const auto& name : item.names) {
            out += " " + name.name;
        }
        out += item.side_effect ? "" : ")";
        return;
    case ItemKind::TypeAlias:
        out += "(type " + item.name + " " + type_text(*item.type) + ")";
        return;
    case ItemKind::Const:
        out += "(const " + item.name;
        if (item.type) {
            out += " : " + type_text(*item.type);
        }
        out += " " + dump(*item.init) + ")";
        return;
    case ItemKind::Component:
        out += (item.exported ? "(export-component " : "(component ") + item.name;
        if (item.has_props) {
            out += " (props ";
            if (item.props_destructured) {
                out += "{";
                for (std::size_t i = 0; i < item.props_bindings.size(); ++i) {
                    const auto& binding = item.props_bindings[i];
                    out += (i == 0 ? "" : ",") + binding.name;
                    if (binding.default_value) {
                        out += "=" + dump(*binding.default_value);
                    }
                }
                out += "}";
            } else {
                out += item.props_name;
            }
            out += " : " + type_text(*item.props_type) + ")";
        }
        if (item.type) {
            out += " (returns " + type_text(*item.type) + ")";
        }
        out += " (body";
        for (const auto& statement : item.body) {
            dump_statement(out, statement);
        }
        out += ") (return " + dump(*item.returned) + "))";
        return;
    case ItemKind::Unknown:
        out += "(unknown)";
        return;
    }
}

} // namespace

std::string dump(const TypeNode& type) {
    return type_text(type);
}

std::string dump(const Module& module) {
    std::string out = "(module";
    for (const auto& item : module.items) {
        dump_item(out, item);
    }
    out += ")";
    return out;
}

} // namespace tessera::compiler::jsx
