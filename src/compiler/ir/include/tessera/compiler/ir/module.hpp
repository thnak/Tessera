// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/component.hpp"

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

struct StylesheetRef {
    std::string id;
    std::string path;
    std::string hash;
};

// docs/specs/ir-v1.md section 1.
struct IrModule {
    int ir_version = kIrVersion;
    std::string name;
    std::vector<std::string> imports;
    std::vector<Component> components;
    std::vector<Binding> bindings;
    std::vector<StylesheetRef> stylesheets;
};

} // namespace tessera::compiler::ir
