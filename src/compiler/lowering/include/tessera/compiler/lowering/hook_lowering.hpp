// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/module_ast.hpp"
#include "tessera/compiler/jsx/source_map.hpp"
#include "tessera/compiler/lowering/type_alias.hpp"
#include "tessera/compiler/rules/component_analysis.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Slice 6 of docs/design/jsx-ir-lowering.md: lower the hook declarations and `bind::` bindings of
// one analysed component to the IR component data of docs/specs/ir-v1.md sections 2, 2.1 and 6.
// JSX nodes, props and cross-file components are later slices; this API never reads the returned
// element.
namespace tessera::compiler::lowering {

// Slice 9: a component tag that the component being lowered may reference (declared in the same
// file, or imported from another project file). `id` is the IR component id of the declaration.
struct ComponentRef {
    std::string name; // tag as written
    std::string id;   // `comp::<Name>`
    // True for a component bound by an `import` statement. An imported name shadows an intrinsic
    // element of the same name in that file (issue #291, jsx-subset-v1.md section 4); a component
    // declared in the same file does not.
    bool imported = false;
};

// One component: its AST item, the analysis of that item, and where its spans live. The analysis
// must be the one produced from `component`; `source_map` must be the map of the same source.
struct HookLoweringContext {
    const jsx::ModuleItem& component;
    const rules::ComponentAnalysis& analysis;
    const jsx::SourceMap& source_map;
    std::string_view file; // project-relative, POSIX separators
    // Slice 9: the component tags this component may reference, local first and imported after.
    // Empty for the single-component entry points, where any component tag is rejected.
    std::span<const ComponentRef> component_refs{};
    // Slice 4: the module the component was parsed from, used to resolve its props type alias
    // (`props: Props` -> `type Props = {...}`). Null in the single-component entry points, where a
    // component's declared prop records then stay unlowered (the documented gap).
    const jsx::Module* module = nullptr;
    // Slice 4 (cross-file): every type name visible in the project files, resolved through imports
    // and re-exports by `lower_project` (issue #70). Null keeps the same-file-only alias walk.
    const TypeNameTable* types = nullptr;
    // The component's declared prop records, lowered before its handlers and nodes so a callback
    // prop can be forwarded or called with its declared type (issue #286). Null when the props type
    // has no resolvable object type.
    const std::vector<ir::Prop>* declared_props = nullptr;
};

// What an event attribute value resolves to (docs/specs/ir-v1.md sections 2.1 and 6).
enum class HandlerRefKind {
    Binding,       // a `// Note: bind::<id>` native handler: `bind::<id>`
    InlineHandler, // a declared setter-call arrow: `handler::N`
};

struct HandlerRef {
    HandlerRefKind kind = HandlerRefKind::InlineHandler;
    std::string id;
};

// A declared name and the IR id it resolves to. The slices after 6 look these up instead of
// rebuilding the analysis.
struct NamedRef {
    std::string name;
    std::string id;
};

struct NamedSetter {
    std::string setter;   // as written, `setCount`
    std::string state_id; // `state::count`
};

struct NamedHandler {
    std::string name;
    HandlerRef ref;
};

// The IR data of one component's hooks and bindings. Everything is in the order
// docs/specs/ir-v1.md section 11 requires: declarations in source order, `bindings` sorted by id.
struct LoweredHooks {
    std::string component_id; // `comp::<Name>`
    std::vector<ir::State> state;
    std::vector<ir::Handler> handlers; // inline setter handlers, `handler::N` from 1
    std::vector<ir::Effect> effects;
    std::vector<ir::Ref> refs; // `target` stays empty until the node lowering of slice 7
    std::vector<ir::AnimatedValue> animated_values;
    std::vector<ir::Binding> bindings; // event, effect and `_cleanup` bindings, sorted by id

    // Seams for the node lowering of slice 7 (attributes `ref`, `onX` and `handler::N` numbering).
    std::vector<NamedRef> ref_index;         // `useRef` names
    std::vector<NamedRef> animated_index;    // `useAnimatedValue` names
    std::vector<NamedSetter> setter_index;   // `useState` setters
    std::vector<NamedHandler> handler_index; // declared handler names

    // The next `handler::N` number; slice 7 may build records itself and keep this in step.
    unsigned next_handler_number = 1;

    [[nodiscard]] const NamedRef* find_ref(std::string_view name) const;
    [[nodiscard]] const NamedRef* find_animated(std::string_view name) const;
    [[nodiscard]] const NamedSetter* find_setter(std::string_view name) const;
    [[nodiscard]] const HandlerRef* find_handler(std::string_view name) const;

    // Fills the `target` of the ref declared as `name` after slice 7 has assigned the node id.
    // False when no `useRef` with that name was lowered.
    [[nodiscard]] bool set_ref_target(std::string_view name, std::string node_id);

    // Appends an inline handler record for a handler written directly in JSX (slice 7) and
    // returns its `handler::N` id. The numbering continues the source-order sequence started by
    // the declared handlers, which is safe because declarations precede the returned element.
    [[nodiscard]] std::string add_handler(std::vector<ir::Param> params,
                                          std::vector<ir::Action> actions, ir::SourceSpan source);
};

using HookLoweringErrors = std::vector<jsx::Diagnostic>;
using HookLoweringResult = std::expected<LoweredHooks, HookLoweringErrors>;
using HandlerActionsResult = std::expected<std::vector<ir::Action>, HookLoweringErrors>;

// Lowers one component. Preconditions: `analysis.item == &component` and `analysis.ok`; calling
// it for a component the analysis rejected returns one TESSERA_IR_MALFORMED and no IR. Any error
// stops the lowering, so the result is either complete component data or diagnostics, never a
// partially built component.
[[nodiscard]] HookLoweringResult lower_hooks(const HookLoweringContext& context);

// Lowers the statements of a setter-call arrow (docs/specs/jsx-subset-v1.md section 9.2) to IR
// actions. `lower_hooks` uses it for declared inline handlers; slice 7 uses it for an arrow
// written directly as an event value. The arrow's parameters resolve to themselves, so a value
// may read them. A statement that is not a setter call or an animated-value `.set(...)` is a
// TESSERA_IR_UNSUPPORTED.
[[nodiscard]] HandlerActionsResult lower_handler_actions(const jsx::Expr& arrow,
                                                         const HookLoweringContext& context);

// Checks every `callProp` action of one handler against the declared props: the prop exists and is
// a callback, a `() => void` prop is called with no argument, and a payload prop is called with the
// handler's own parameter of that record type. `params` are the handler's typed parameters.
// Returns the first violation.
[[nodiscard]] std::optional<jsx::Diagnostic>
check_callback_calls(const HookLoweringContext& context, std::span<const ir::Param> params,
                     std::span<const ir::Action> actions, jsx::Span span);

} // namespace tessera::compiler::lowering
