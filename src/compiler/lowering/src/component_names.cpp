// SPDX-License-Identifier: MIT

// Slice 5's NameResolver seam, implemented from the analysis symbol table (docs/design/
// jsx-ir-lowering.md section 3), plus decision 8's local table.

#include "tessera/compiler/lowering/component_names.hpp"

#include <cstddef>
#include <utility>

namespace tessera::compiler::lowering {

// Decision 8: the initializer a `const` local stands for. A plain `const x = e;` inlines `e`; an
// object pattern `const { state } = io;` inlines `io.state`, so the field is kept beside the
// initializer. An array pattern (`const [a, b] = e;`) has no single initializer per name, so it
// gets no entry and stays unresolved rather than inlining half of the pattern.
void ComponentNames::record_const_local(const jsx::Statement& statement, std::string_view name) {
    if (statement.init == nullptr) {
        return;
    }
    if (!statement.object_pattern.empty()) {
        // An object pattern binds each listed field of the initializer to a local of the same name
        // (the parser refuses a rename outside the experimental custom-hook path), so the local
        // name is the field name and the inlined reference appends it to the initializer's id.
        const_locals_.push_back({std::string(name), statement.init.get(), std::string(name)});
        return;
    }
    if (statement.array_pattern) {
        return;
    }
    const_locals_.push_back({std::string(name), statement.init.get(), std::string()});
}

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
        case rules::SymbolKind::Environment:
            // `const bp = useBreakpoint();` reads `env::bp` (docs/specs/jsx-subset-v1.md section
            // 7.1, issue #692). Without this the reference pass reported the value as an unresolved
            // name, so no component could read an environment hook.
            entries_.push_back({symbol.name, "env::" + symbol.name});
            break;
        case rules::SymbolKind::PropsParam:
            if (props_param_.empty()) {
                props_param_ = symbol.name;
            }
            break;
        case rules::SymbolKind::Local:
            // Decision 8: a local is inlined, never referenced, so it is recorded in its own table
            // instead of `entries_`. The analysis already gives the statement that declares it.
            if (analysis.item != nullptr && symbol.statement < analysis.item->body.size()) {
                record_const_local(analysis.item->body[symbol.statement], symbol.name);
            }
            break;
        case rules::SymbolKind::StateSetter:
        case rules::SymbolKind::Handler:
            // Function values have no IR expression.
            break;
        }
    }
    // A behavior hook call that returns outputs makes each output readable as `<local>.<field>`
    // (behavior-hooks-v1.md section 6). Recorded here rather than in `entries_` because the
    // reference is dotted: the base name alone is not a value, so `press` on its own must stay
    // unresolved while `press.pressed` resolves.
    for (const rules::BehaviorInfo& behavior : analysis.behaviors) {
        if (behavior.name.empty() || behavior.outputs.empty()) {
            continue;
        }
        behavior_outputs_.push_back({behavior.name, behavior.outputs});
    }
}

void ComponentNames::add_local(std::string name) {
    locals_.push_back(std::move(name));
}

void ComponentNames::add_const_local(std::string name, const jsx::Expr* initializer) {
    if (initializer == nullptr) {
        return;
    }
    const_locals_.push_back({std::move(name), initializer, std::string()});
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
    // ADR-066 item 3: a field of a record-typed state, `state.name`. The state itself is an entry
    // (`state` -> `state::state`), so the dotted form is the entry's id plus the field. The IR
    // validator checks the field against the record type, so an unknown field is reported there
    // rather than here.
    if (dot != std::string_view::npos) {
        const std::string_view base = source_name.substr(0, dot);
        for (const Entry& entry : entries_) {
            if (entry.name == base) {
                return entry.id + std::string(source_name.substr(dot));
            }
        }
    }
    // A behavior output: `<local>.<field>` resolves to `beh::<local>.<field>` (behavior-hooks-v1.md
    // section 6). Only a field the hook actually returns resolves; `press.nope` stays unresolved so
    // the IR validator reports it rather than the generator inventing an accessor.
    for (const BehaviorOutputs& behavior : behavior_outputs_) {
        if (source_name.size() <= behavior.name.size() + 1 ||
            source_name.compare(0, behavior.name.size(), behavior.name) != 0 ||
            source_name[behavior.name.size()] != '.') {
            continue;
        }
        const std::string_view field = source_name.substr(behavior.name.size() + 1);
        for (const std::string& output : behavior.outputs) {
            if (output == field) {
                return "beh::" + behavior.name + "." + std::string(field);
            }
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

std::optional<LocalBinding> ComponentNames::local_binding(std::string_view source_name) const {
    // A handler parameter or a loop variable shadows a local of the same name (decision 8), and
    // `resolve` answers it as an action local, so it is not inlined here.
    for (const std::string& local : locals_) {
        if (local == source_name) {
            return std::nullopt;
        }
    }
    // `row.title` for `const row = props.row;`: the base is the local, the rest is the field to
    // append to whatever the initializer lowers to.
    std::string_view base = source_name;
    std::string_view field;
    if (const std::size_t dot = source_name.find('.'); dot != std::string_view::npos) {
        base = source_name.substr(0, dot);
        field = source_name.substr(dot + 1);
    }
    for (const ConstLocal& local : const_locals_) {
        if (local.name != base) {
            continue;
        }
        LocalBinding binding;
        binding.initializer = local.initializer;
        // The pattern's own field comes first: `const { state } = io;` then `state.x` reads
        // `io.state.x`.
        binding.field = local.field;
        if (!field.empty()) {
            if (!binding.field.empty()) {
                binding.field += '.';
            }
            binding.field += field;
        }
        return binding;
    }
    return std::nullopt;
}

} // namespace tessera::compiler::lowering
