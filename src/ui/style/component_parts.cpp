// SPDX-License-Identifier: MIT

#include "tessera/ui/style/component_parts.hpp"
#include "tessera/ui/style/selector.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

// `TESSERA_CSS_UNDECLARED_PART` (stylesheet-v1 section 15).
std::string undeclared_part_message(std::string_view component, std::string_view part,
                                    const ComponentParts& declared) {
    std::string message = "`::part(";
    message.append(part);
    message.append(")` is not a declared part of the library component `");
    message.append(component);
    message.push_back('`');
    if (declared.parts.empty()) {
        message.append("; it declares no parts");
    } else {
        message.append("; declared parts:");
        for (const std::string& name : declared.parts) {
            message.push_back(' ');
            message.append(name);
        }
    }
    return message;
}

// A rule with no declarations is dropped during preparation, so this is only a guard.
std::size_t rule_offset(const std::vector<Declaration>& declarations) {
    return declarations.empty() ? 0 : declarations.front().offset;
}

} // namespace

void ComponentPartRegistry::declare(std::string name, std::span<const std::string_view> parts) {
    ComponentParts entry;
    entry.name = std::move(name);
    entry.parts.reserve(parts.size());
    for (const std::string_view part : parts) {
        if (!part.empty()) {
            entry.parts.emplace_back(part);
        }
    }
    std::sort(entry.parts.begin(), entry.parts.end());
    entry.parts.erase(std::unique(entry.parts.begin(), entry.parts.end()), entry.parts.end());

    const auto position =
        std::lower_bound(components_.begin(), components_.end(), entry.name,
                         [](const ComponentParts& candidate, const std::string& wanted) {
                             return candidate.name < wanted;
                         });
    if (position != components_.end() && position->name == entry.name) {
        *position = std::move(entry);
        return;
    }
    components_.insert(position, std::move(entry));
}

void ComponentPartRegistry::clear() {
    components_.clear();
}

bool ComponentPartRegistry::declares_component(std::string_view name) const {
    return find(name) != nullptr;
}

bool ComponentPartRegistry::declares_part(std::string_view component, std::string_view part) const {
    const ComponentParts* declared = find(component);
    if (declared == nullptr) {
        return false;
    }
    return std::binary_search(declared->parts.begin(), declared->parts.end(), part);
}

const ComponentParts* ComponentPartRegistry::find(std::string_view name) const {
    const auto position =
        std::lower_bound(components_.begin(), components_.end(), name,
                         [](const ComponentParts& candidate, std::string_view wanted) {
                             return candidate.name < wanted;
                         });
    if (position == components_.end() || position->name != name) {
        return nullptr;
    }
    return &*position;
}

std::vector<StyleDiagnostic> check_part_selectors(const PreparedStyleSheet& prepared,
                                                  const ComponentPartRegistry& parts) {
    std::vector<StyleDiagnostic> diagnostics;
    // One diagnostic per (component, part name); the key is the message, which names both.
    std::vector<std::string> reported;
    for (const PreparedItem& item : prepared.items) {
        if (item.kind != PreparedItemKind::Rule) {
            continue;
        }
        for (const ComplexSelector& selector : item.selectors.selectors) {
            if (selector.compounds.empty()) {
                continue;
            }
            const CompoundSelector& subject = selector.compounds.back();
            const std::size_t part_index = part_pseudo_index(subject);
            if (part_index == subject.pseudos.size() || subject.tag.empty()) {
                continue;
            }
            const ComponentParts* declared = parts.find(subject.tag);
            if (declared == nullptr) {
                continue; // an app component (or `*`): no published part contract to check
            }
            const PseudoClass& pseudo = subject.pseudos[part_index];
            for (std::size_t name_index = 0;; ++name_index) {
                const std::string_view name = part_name_at(pseudo.argument, name_index);
                if (name.empty()) {
                    break;
                }
                if (parts.declares_part(subject.tag, name)) {
                    continue;
                }
                std::string message = undeclared_part_message(subject.tag, name, *declared);
                if (std::find(reported.begin(), reported.end(), message) != reported.end()) {
                    continue;
                }
                StyleDiagnostic diagnostic;
                diagnostic.kind = StyleDiagnosticKind::UndeclaredPart;
                diagnostic.offset = rule_offset(item.declarations);
                diagnostic.property = std::string(subject.tag);
                diagnostic.message = std::move(message);
                diagnostics.push_back(std::move(diagnostic));
                reported.push_back(diagnostics.back().message);
            }
        }
    }
    return diagnostics;
}

} // namespace tessera::ui::style
