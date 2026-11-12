// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/node.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Internal seam of the codegen slice. Not installed; only src/compiler/codegen includes it.
namespace tessera::compiler::codegen::internal {

// Deterministic line-oriented C++ writer: two-space indent, LF only, one trailing newline. It never
// runs clang-format, so output depends only on the IR (docs/design/codegen.md).
struct CppWriter {
    std::string text;
    int indent = 0;

    void line(std::string_view text);
    void open(std::string_view text);         // line + "{"
    void close(std::string_view suffix = {}); // "}" or "};"
};

// A C++ type for an IR type (docs/specs/ir-v1.md section 2). Null when the type needs a runtime
// type this slice does not have; the caller reports TESSERA_CODEGEN_UNSUPPORTED.
[[nodiscard]] std::optional<std::string> cpp_type(std::string_view ir_type);

// C++ literal text for one IR literal (string literals are escaped).
[[nodiscard]] std::string literal_text(const ir::LiteralValue& literal);

// The generated member name for a reference id (`state::count` -> `state_count_`).
[[nodiscard]] std::string member_name(std::string_view ir_id);

// The `AnimatedScalar` controller behind an animated cell (`anim::x` -> `anim_x_scalar_`). The
// plain `anim_x_` field keeps the sampled value every expression reads.
[[nodiscard]] std::string animated_controller_name(std::string_view ir_id);

// The member that remembers which handler to run when the animated cell's current set completes
// (`anim::x` -> `anim_x_on_complete_`): the 1-based index into `Component::handlers`, 0 for none.
[[nodiscard]] std::string animated_on_complete_name(std::string_view ir_id);

// True when any handler action of `component` carries an `onComplete` for the animated cell.
[[nodiscard]] bool animated_has_completion(const ir::Component& component, std::string_view ir_id);

// One loop local (`item`, `index`) visible inside a generated list-item builder: the IR reference
// id the template expressions use, the C++ expression that names the value, and its IR element
// type. Generated item bodies bind the item by `const T&` and the index as `std::size_t`; the C++
// form is what expression emission substitutes for the IR reference.
struct LocalBinding {
    std::string name;
    std::string cpp;
    std::string ir_type;
};
using LocalBindings = std::vector<LocalBinding>;

// The binding for an IR reference id inside a list template, or nullptr outside one.
[[nodiscard]] const LocalBinding* find_local(const LocalBindings& locals, std::string_view id);

// Emits one IR expression as C++ source; `locals` supplies the loop item/index of a list template.
// A nullopt `text` carries the error message in `error`.
struct ExprEmission {
    std::string text;
    std::string error;
};
[[nodiscard]] ExprEmission emit_expr(const ir::Expr& expr, const ir::Component& component,
                                     const LocalBindings& locals = {});

// Emits an initializer expression: a state/prop/animated initial value position, where an IR array
// literal becomes a braced C++ list (`std::vector<std::string> items_ = {"a", "b"};`). Everywhere
// else `emit_expr` rejects an array literal loudly.
[[nodiscard]] ExprEmission emit_initializer_expr(const ir::Expr& expr,
                                                 const ir::Component& component);

// Emits the body of an inline handler or preview: one line per action.
[[nodiscard]] std::optional<std::string> emit_actions(const std::vector<ir::Action>& actions,
                                                      const ir::Component& component,
                                                      std::span<const ir::Param> params,
                                                      std::string& error);

// One interpolated text node: ordinal matches the generated node_N variables.
struct DynamicText {
    unsigned ordinal = 0;
    const ir::Expr* text = nullptr;
};

// One generated component instance: a `Component` node of the emitted component (pre-order ordinal
// matches the generated node_N variables) and the referenced component.
struct ComponentInstance;

// One prop passed to an instance (`docs/specs/component-model-v1.md` section 3): an ordinary value
// prop (`value`), or a record-typed callback prop resolved to a handler (`callback`).
struct PropBinding {
    std::string name;                // declared prop name
    std::string type;                // declared IR type
    const ir::Expr* value = nullptr; // value prop
    std::string callback;            // generated callable name of a callback prop

    [[nodiscard]] bool is_callback() const {
        return value == nullptr;
    }
};

struct ComponentInstance {
    unsigned ordinal = 0;
    std::string component_id; // IR id, `comp::<Name>`
    std::string type_name;    // generated C++ struct name (the component name)
    bool takes_children = false;
    std::vector<PropBinding> props; // declared prop order, then callback props
};

struct NodeEmitState; // emit_node.hpp

// The generated form-state binding of one component (form_binding.hpp). The node emitter consults
// it to store a bound Field's slot widget and to bind the generated edit event.
struct FormPlan;

// The generated `node_N` id expression for a node (the IR `uiId` when numeric, otherwise the
// pre-order placeholder; inside a list item the template id is offset by the item index).
[[nodiscard]] std::string node_id_text(const ir::Node& node, NodeEmitState& state,
                                       std::string& error);

// True when the component declares the static `children` slot (ir-v1.md section 3.4).
[[nodiscard]] bool component_takes_children(const ir::Component& component);

// The module's component record for an IR component id, or nullptr when the id is unknown.
[[nodiscard]] const ir::Component* find_module_component(const ir::IrModule& module,
                                                         std::string_view id);

// The generated struct member that owns a referenced component instance.
[[nodiscard]] std::string instance_member(unsigned ordinal);

// Collects the `Component` nodes of one component's tree in pre-order; sets `error` for the
// deferred forms (a conditional or looped instance, component props, an instance inside a list
// template) and returns false.
[[nodiscard]] bool collect_component_instances(const ir::IrModule& module,
                                               const ir::Component& component,
                                               std::vector<ComponentInstance>& instances,
                                               std::string& error);

// The generated component headers a component's source/header must include, sorted by name.
[[nodiscard]] std::vector<std::string> component_includes(const ir::IrModule& module,
                                                          const ir::Component& component);

// Emits one `Component` node: its host node, the caller's slot content, and the instance's
// build() call (emit_composition.cpp).
[[nodiscard]] bool emit_component_node(CppWriter& writer, const ir::Node& node,
                                       NodeEmitState& state, std::string& variable,
                                       std::string& error);

// Rejects a reference cycle in the component graph before any file is emitted (a cycle would make
// the generated structs infinitely recursive); true when a cycle was reported.
[[nodiscard]] bool reject_component_cycle(const ir::IrModule& module, std::string& error);

// An interpolated element metadata value (`id`, `className`, `role`, `aria-*`, `automationId`).
// The generated component owns the current string (node metadata stays a borrowed view) and only
// re-applies it when the hash changes; id/className mark Dirty::Style through the node setters.
enum class MetadataField { ElementId, ClassName, Role, Aria, Style };

struct ControlledInput;
struct DynamicPlan;

struct DynamicMetadata {
    unsigned ordinal = 0;
    MetadataField field = MetadataField::ElementId;
    std::string name; // the aria attribute name, for MetadataField::Aria
    const ir::Expr* value = nullptr;
};

// Emits the component header/source (emit_header.cpp / emit_component.cpp). `module` supplies the
// referenced components for composition and the `children` slot.
[[nodiscard]] bool emit_component_header(const ir::Component& component, const ir::IrModule& module,
                                         const std::vector<const ir::Binding*>& bindings,
                                         const std::vector<DynamicText>& dynamic_text,
                                         const std::vector<DynamicMetadata>& dynamic_metadata,
                                         const std::vector<ControlledInput>& controlled_inputs,
                                         const DynamicPlan& plan, const FormPlan& forms,
                                         const std::vector<ComponentInstance>& instances,
                                         GeneratedFile& file, std::string& error);

// Emits the effect deps-hash helpers, mount/update_effects/unmount and the child-instance
// lifecycle propagation (emit_lifecycle.cpp).
[[nodiscard]] bool emit_lifecycle(const ir::Component& component,
                                  const std::vector<const ir::Binding*>& bindings,
                                  const std::vector<ComponentInstance>& instances,
                                  CppWriter& writer, std::string& error);

// Appends `<Component>.hpp` and `<Component>.cpp` to `files`, or sets `error` and returns false.
[[nodiscard]] bool emit_component(const ir::Component& component, const ir::IrModule& module,
                                  std::vector<GeneratedFile>& files, std::string& error);

// One stylesheet of the module that the caller supplied a blob for, in `module.stylesheets` order.
struct ResolvedStylesheet {
    std::string_view id;
    std::string_view path;
    std::span<const std::uint8_t> bytes;
};

// The C++ identifier suffix of a `StylesheetRef::id` (`css::ui/app` -> `css_ui_app`): every run of
// non-alphanumeric characters becomes one `_` (emit_stylesheet_embed.cpp).
[[nodiscard]] std::string stylesheet_identifier(std::string_view id);

// Emits `<module.name>_stylesheets.hpp` (docs/specs/compiled-stylesheet-v1.md section 9): one
// `inline constexpr` array per resolved sheet, then the `kEmbeddedStylesheets` table of the
// engine's `tessera::ui::style::EmbeddedStylesheet` records, in `sheets` order. Deterministic: same
// sheets, same bytes.
[[nodiscard]] std::string emit_stylesheet_header(std::string_view module_name,
                                                 std::span<const ResolvedStylesheet> sheets);

// Emits `<module.name>_theme.hpp` (docs/specs/compiled-stylesheet-v1.md "Theme asset blob"): one
// `inline constexpr` byte array named `kThemeBlob` and the `kEmbeddedTheme` record of the engine's
// `tessera::ui::style::EmbeddedTheme`. Deterministic: same bytes, same file.
[[nodiscard]] std::string emit_theme_header(std::string_view module_name,
                                            std::span<const std::uint8_t> bytes);

// `Path` -> `PaintKind::Vector` (emit_vector.hpp): the synthesized SVG asset of every `Path` node
// and the `<module.name>_vector_assets.hpp` file that embeds them as an `AssetBundle`.
struct VectorAsset;
[[nodiscard]] bool collect_vector_assets(const ir::IrModule& module,
                                         std::vector<VectorAsset>& assets, std::string& error);
[[nodiscard]] std::string emit_vector_assets_header(std::string_view module_name,
                                                    std::span<const VectorAsset> assets);

} // namespace tessera::compiler::codegen::internal
