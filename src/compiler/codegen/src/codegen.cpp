// SPDX-License-Identifier: MIT

// Entry point and deterministic writer of the first C++ codegen slice.

#include "codegen_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen {
namespace {

bool valid_namespace(std::string_view name) {
    if (name.empty() || (std::isdigit(static_cast<unsigned char>(name.front())) != 0)) {
        return false;
    }
    for (const char c : name) {
        const auto ch = static_cast<unsigned char>(c);
        if (std::isalnum(ch) == 0 && c != '_') {
            return false;
        }
    }
    return true;
}

CodegenErrors single(std::string message) {
    CodegenErrors errors;
    errors.push_back(std::move(message));
    return errors;
}

} // namespace

namespace internal {

void CppWriter::line(std::string_view line_text) {
    text.append(static_cast<std::size_t>(indent) * 2, ' ');
    text += line_text;
    text += '\n';
}

void CppWriter::open(std::string_view header_text) {
    std::string header(header_text);
    header += " {";
    line(header);
    ++indent;
}

void CppWriter::close(std::string_view suffix) {
    --indent;
    std::string closing("}");
    closing += suffix;
    line(closing);
}

} // namespace internal

CodegenResult generate(const ir::IrModule& module) {
    if (!valid_namespace(module.name)) {
        return std::unexpected(single(
            "TESSERA_CODEGEN_UNSUPPORTED: module.name `" + module.name +
            "` is not a C++ namespace identifier; decision 14 takes it from the project config"));
    }
    std::vector<const ir::Component*> components;
    components.reserve(module.components.size());
    for (const ir::Component& component : module.components) {
        components.push_back(&component);
    }
    std::sort(components.begin(), components.end(),
              [](const ir::Component* left, const ir::Component* right) {
                  return left->name < right->name;
              });
    for (std::size_t i = 1; i < components.size(); ++i) {
        if (components[i - 1]->name == components[i]->name) {
            return std::unexpected(single("TESSERA_CODEGEN_UNSUPPORTED: duplicate component `" +
                                          components[i]->name + "`"));
        }
    }
    std::vector<GeneratedFile> files;
    std::string cycle_error;
    if (internal::reject_component_cycle(module, cycle_error)) {
        return std::unexpected(single(std::move(cycle_error)));
    }
    for (const ir::Component* component : components) {
        std::string error;
        if (!internal::emit_component(*component, module, files, error)) {
            return std::unexpected(single(std::move(error)));
        }
    }
    return files;
}

} // namespace tessera::compiler::codegen
