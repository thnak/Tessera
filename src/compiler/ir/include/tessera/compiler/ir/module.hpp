// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::ir {

inline constexpr int kIrVersion = 1;

enum class BindingKind { State, Effect, Ref, Animated, Event, Logic };

std::string_view binding_kind_name(BindingKind kind);

struct Preview {
    std::vector<Action> actions;
    ExprPtr return_value; // optional
};

// docs/specs/ir-v1.md section 6.
struct Binding {
    std::string id;
    std::string component;
    BindingKind kind = BindingKind::Event;
    SourceSpan source;
    std::string region;
    std::vector<Param> params;
    std::string return_type; // empty means void
    std::optional<Preview> preview;
};

// Issue #463: one class a `*.module.css` declares, as authored and as scoped. The inspector shows
// `local` while the selector index sees `scoped`.
struct StylesheetClass {
    std::string local;
    std::string scoped;
};

struct StylesheetRef {
    std::string id;
    std::string path;
    // FNV-1a of the file bytes: it identifies the source file, not the scoped rewrite.
    std::string hash;
    // Issue #463: the class map a CSS module carries, sorted by `local` and unique. Omitted from
    // the JSON when empty, so a project with no `.module.css` stays byte-identical (the same rule
    // as `types`, section 2.2).
    std::vector<StylesheetClass> classes;
};

// docs/specs/ir-v1.md section 1.
struct IrModule {
    int ir_version = kIrVersion;
    std::string name;
    std::vector<std::string> imports;
    std::vector<Component> components;
    std::vector<Binding> bindings;
    std::vector<StylesheetRef> stylesheets;
    // Section 2.2 (ADR-051): the declared record types, sorted by name. Optional and additive: a
    // module with no records writes no `types` field and is byte-identical to the IR before it.
    std::vector<RecordType> types;
};

} // namespace tessera::compiler::ir
