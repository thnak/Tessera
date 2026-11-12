// SPDX-License-Identifier: MIT

// Entry point and deterministic writer of the first C++ codegen slice.

#include "codegen_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <span>
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

// The first unknown embed path in sorted order: the caller's embed order is not part of the
// contract, so the diagnostic must not depend on it.
std::optional<std::string> first_unknown_embed(const ir::IrModule& module,
                                               std::span<const StylesheetEmbed> embeds) {
    std::vector<std::string_view> unknown;
    for (const StylesheetEmbed& embed : embeds) {
        const bool known =
            std::any_of(module.stylesheets.begin(), module.stylesheets.end(),
                        [&](const ir::StylesheetRef& sheet) { return sheet.path == embed.path; });
        if (!known) {
            unknown.push_back(embed.path);
        }
    }
    if (unknown.empty()) {
        return std::nullopt;
    }
    return std::string(*std::min_element(unknown.begin(), unknown.end()));
}

// The first embed naming `path`, or nullptr. A duplicate path keeps the caller's first blob, so the
// result stays deterministic for a given embed list.
const StylesheetEmbed* find_embed(std::span<const StylesheetEmbed> embeds, std::string_view path) {
    for (const StylesheetEmbed& embed : embeds) {
        if (embed.path == path) {
            return &embed;
        }
    }
    return nullptr;
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

std::vector<std::string> codegen_warnings(const ir::IrModule& module,
                                          std::span<const StylesheetEmbed> embeds) {
    std::vector<std::string> warnings;
    for (const ir::StylesheetRef& sheet : module.stylesheets) {
        if (find_embed(embeds, sheet.path) != nullptr) {
            continue;
        }
        warnings.push_back("TESSERA_CG_STYLESHEET_NOT_EMBEDDED: stylesheet `" + sheet.path +
                           "` is not embedded in the generated code; apply it at startup with "
                           "compile_stylesheet() or the tessera.json `ui.styles` list");
    }
    return warnings;
}

CodegenResult generate(const ir::IrModule& module, std::span<const StylesheetEmbed> embeds) {
    if (!valid_namespace(module.name)) {
        return std::unexpected(single(
            "TESSERA_CODEGEN_UNSUPPORTED: module.name `" + module.name +
            "` is not a C++ namespace identifier; decision 14 takes it from the project config"));
    }
    if (const std::optional<std::string> unknown = first_unknown_embed(module, embeds)) {
        return std::unexpected(single("TESSERA_CODEGEN_STYLESHEET_UNKNOWN: stylesheet blob for `" +
                                      *unknown + "` is not a stylesheet of this module"));
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
    std::vector<internal::ResolvedStylesheet> resolved;
    resolved.reserve(module.stylesheets.size());
    for (const ir::StylesheetRef& sheet : module.stylesheets) {
        const StylesheetEmbed* embed = find_embed(embeds, sheet.path);
        if (embed == nullptr) {
            continue;
        }
        resolved.push_back(internal::ResolvedStylesheet{
            sheet.id, sheet.path, std::span<const std::uint8_t>(embed->bytes)});
    }
    if (!resolved.empty()) {
        files.push_back(GeneratedFile{module.name + "_stylesheets.hpp",
                                      internal::emit_stylesheet_header(module.name, resolved)});
    }
    return files;
}

} // namespace tessera::compiler::codegen
