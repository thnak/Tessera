// SPDX-License-Identifier: MIT

// Slice 5's NameResolver seam, implemented from the analysis symbol table (docs/design/
// jsx-ir-lowering.md section 3).

#include "tessera/compiler/lowering/component_names.hpp"

#include <utility>

namespace tessera::compiler::lowering {

ComponentNames::ComponentNames(const rules::ComponentAnalysis& analysis) {
    for (const rules::Symbol& symbol : analysis.symbols) {
        switch (symbol.kind) {
        case rules::SymbolKind::Prop:
            entries_.push_back({symbol.name, "prop::" + symbol.name});
            break;
        case rules::SymbolKind::State:
            entries_.push_back({symbol.name, "state::" + symbol.name});
            break;
        case rules::SymbolKind::Ref:
            entries_.push_back({symbol.name, "ref::" + symbol.name});
            break;
        case rules::SymbolKind::AnimatedValue:
            entries_.push_back({symbol.name, "anim::" + symbol.name});
            break;
        case rules::SymbolKind::PropsParam:
            if (props_param_.empty()) {
                props_param_ = symbol.name;
            }
            break;
        case rules::SymbolKind::StateSetter:
        case rules::SymbolKind::Handler:
        case rules::SymbolKind::Local:
            // Function values have no IR expression; locals are inlined later (decision 8).
            break;
        }
    }
}

void ComponentNames::add_local(std::string name) {
    locals_.push_back(std::move(name));
}

void ComponentNames::truncate_locals(std::size_t count) {
    if (count < locals_.size()) {
        locals_.resize(count);
    }
}

std::optional<std::string> ComponentNames::resolve(std::string_view source_name) const {
    for (const std::string& local : locals_) {
        if (local == source_name) {
            return std::string(source_name);
        }
    }
    // A field of a local (a handler parameter): `e.value` keeps the dotted id, as a loop item does
    // (decision 8). The IR validator resolves `e` against the action locals.
    const std::size_t dot = source_name.find('.');
    if (dot != std::string_view::npos) {
        const std::string_view base = source_name.substr(0, dot);
        for (const std::string& local : locals_) {
            if (local == base) {
                return std::string(source_name);
            }
        }
    }
    for (const Entry& entry : entries_) {
        if (entry.name == source_name) {
            return entry.id;
        }
    }
    // `props.x` of a non-destructured parameter is a prop reference (decision 8). Nested fields
    // stay dotted (`prop::row.label`); the IR validator checks them against the record type.
    if (!props_param_.empty() && source_name.size() > props_param_.size() + 1 &&
        source_name.compare(0, props_param_.size(), props_param_) == 0 &&
        source_name[props_param_.size()] == '.') {
        return "prop::" + std::string(source_name.substr(props_param_.size() + 1));
    }
    return std::nullopt;
}

} // namespace tessera::compiler::lowering
