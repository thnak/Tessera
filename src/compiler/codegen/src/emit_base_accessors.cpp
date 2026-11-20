// SPDX-License-Identifier: MIT

// ADR-040 generated base, access section: the generated data members are private and developer code
// reads them through generated accessors. This TU owns the member-shape helpers
// (`is_data_member`, `data_member_name`), the `private:`/`public:` partitioning of the struct body
// (`apply_access`), the accessor emission itself (`emit_base_members`) and the binding-shape text
// helpers the base and the scaffold share. `emit_base.cpp` keeps the text reshaping, so both files
// stay under the 500-line hard limit (AGENTS.md, Code conventions).
//
// The split is behavior-preserving: everything here emits the same text in the same order as
// before, which the committed `.g.hpp` goldens prove.

#include "emit_base.hpp"
#include "emit_base_accessors.hpp"
#include "emit_events.hpp"
#include "emit_props.hpp"
#include "emit_behaviors.hpp"
#include "emit_services.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::internal {
namespace {

// True for a generated data member (`state_count_`, `effect_sync_state_`, `prop_cb_`): the declared
// name ends in `_`, which is the writer's convention for every field and for no method.
bool is_data_member(const Item& item) {
    std::string text;
    for (const std::string& line : item.code) {
        text += std::string(trimmed(line)) + " ";
    }
    std::size_t at = 0;
    while (text.compare(at, 2, "[[") == 0) {
        const std::size_t close = text.find("]]", at);
        if (close == std::string::npos) {
            return false;
        }
        at = close + 2;
        while (at < text.size() && text[at] == ' ') {
            ++at;
        }
    }
    int depth = 0;
    std::size_t cut = text.size();
    for (std::size_t i = at; i < text.size(); ++i) {
        const char c = text[i];
        depth += (c == '(' || c == '<') - (c == ')' || c == '>');
        const bool assign = c == '=' && i + 1 < text.size() && text[i + 1] != '=' && i > 0 &&
                            std::string_view("=<>!").find(text[i - 1]) == std::string_view::npos;
        if (depth == 0 && (c == ';' || c == '{' || assign)) {
            cut = i;
            break;
        }
    }
    std::string head = text.substr(at, cut - at);
    while (!head.empty() && head.back() == ' ') {
        head.pop_back();
    }
    if (head.empty() || head.back() == ')' || head.ends_with(" const")) {
        return false;
    }
    return head.back() == '_';
}

// The declared name of a data member (`prop_title_` in `std::string prop_title_ = {};`).
std::string data_member_name(const Item& item) {
    const std::string& line = item.code.front();
    std::size_t end = line.find(" =");
    if (end == std::string::npos) {
        end = line.find(';');
    }
    if (end == std::string::npos) {
        end = line.size();
    }
    std::size_t begin = end;
    while (begin > 0 && is_word(line[begin - 1])) {
        --begin;
    }
    return line.substr(begin, end - begin);
}

} // namespace

std::vector<Item> group_items(const std::vector<std::string>& lines) {
    std::vector<Item> items;
    Item current;
    int depth = 0;
    bool seen_open = false;
    for (const std::string& line : lines) {
        const std::string_view text = trimmed(line);
        if (current.code.empty()) {
            if (text.empty()) {
                continue;
            }
            if (starts_with(text, "//")) {
                current.lead.push_back(line);
                continue;
            }
        }
        current.code.push_back(line);
        const int delta = brace_delta(line);
        depth += delta;
        seen_open = seen_open || line.find('{') != std::string::npos;
        // A declaration ends when its braces balance (a trailing `// namespace` comment must not
        // matter) or, with no brace at all, at its `;`.
        if (depth == 0 && (seen_open || (!text.empty() && text.back() == ';'))) {
            items.push_back(std::move(current));
            current = Item{};
            seen_open = false;
        }
    }
    if (!current.code.empty()) {
        items.push_back(std::move(current));
    }
    return items;
}

// ADR-040 item 2: generated fields and the dispatch thunks are private. Methods stay public (the
// host drives build/mount/update/..., the developer reads accessors), and so do the `prop_` fields:
// a parent component writes the props of the child instance it owns.
std::vector<std::string> apply_access(const std::vector<std::string>& struct_body,
                                      const std::string& friend_line) {
    std::vector<std::string> out;
    bool is_private = false;
    const auto set_access = [&](bool want_private) {
        if (want_private != is_private) {
            out.push_back(want_private ? "    private:" : "    public:");
            is_private = want_private;
        }
    };
    set_access(true);
    if (!friend_line.empty()) {
        out.push_back(friend_line);
    }
    for (const Item& item : group_items(struct_body)) {
        bool wants_private = false;
        if (is_data_member(item)) {
            const std::string name = data_member_name(item);
            // `node_id_base_` is written by the owning component before build (issue #295).
            // The form binding's members are the host's configuration surface: the emitter builds
            // the controller lazily *so that the host can set the config* (a CompletionQueue, a
            // require-touched policy), and reads the report back. They stay public for the same
            // reason the `prop_` fields do.
            // The `kBit_` cell constants are how an owner marks a child's prop dirty (ADR-056).
            wants_private = !starts_with(name, "prop_") && name != "node_id_base_" &&
                            !starts_with(name, "form_") && !starts_with(name, "kBit_");
        } else {
            for (const std::string& line : item.code) {
                wants_private =
                    wants_private || line.find("static_cast<Self&>(*this).") != std::string::npos;
            }
        }
        set_access(wants_private);
        append(out, item.lead);
        append(out, item.code);
    }
    return out;
}

// The accessor/reserved names a generated member already uses.
bool reserved_member(std::string_view name) {
    static constexpr std::string_view kReserved[] = {"build",
                                                     "mount",
                                                     "update",
                                                     "unmount",
                                                     "apply_state",
                                                     "update_effects",
                                                     "snapshot_state",
                                                     "schedule_update",
                                                     "attach_scheduler",
                                                     "detach_scheduler",
                                                     "flush_update",
                                                     "note_dirty",
                                                     "mark_all_dirty"};
    return std::find(std::begin(kReserved), std::end(kReserved), name) != std::end(kReserved);
}

std::optional<BindingShape> binding_shape(const ir::Binding& binding, std::string& error) {
    BindingShape shape;
    shape.return_type = "void";
    if (binding.kind == ir::BindingKind::Event) {
        bool typed = false;
        std::string param_name;
        const std::optional<std::string> signature =
            payload_signature(binding.params, typed, param_name);
        if (!signature) {
            base_fail(error, "binding `" + binding.id + "` has an unsupported payload record");
            return std::nullopt;
        }
        const std::size_t space = signature->rfind(' ');
        shape.event = true;
        shape.param_type = signature->substr(0, space);
        shape.param_name = signature->substr(space + 1);
        return shape;
    }
    if (!binding.return_type.empty()) {
        shape.return_type = handler_type(binding.return_type);
        if (shape.return_type.empty()) {
            base_fail(error, "binding `" + binding.id + "` has an unsupported return type `" +
                                 binding.return_type + "`");
            return std::nullopt;
        }
    }
    return shape;
}

std::string binding_signature(const ir::Binding& binding, const BindingShape& shape) {
    return shape.return_type + " " + binding.region + "(" +
           (shape.event ? shape.param_type + " " + shape.param_name : std::string()) + ")";
}

bool emit_base_members(CppWriter& writer, const ir::IrModule& module,
                       const ir::Component& component,
                       const std::vector<const ir::Binding*>& bindings,
                       const std::vector<ComponentInstance>& instances, std::string& error) {
    std::set<std::string> names;
    const auto claim = [&](const std::string& name) {
        if (reserved_member(name) || !names.insert(name).second) {
            return base_fail(error, "component `" + component.name + "`: accessor `" + name +
                                        "` collides with another generated member");
        }
        return true;
    };
    writer.line("// ADR-040 dispatch thunks: each calls the same-named member the developer class");
    writer.line("// defines; build() static_asserts that it does (static dispatch, no vtable).");
    for (const ir::Binding* binding : bindings) {
        const std::optional<BindingShape> shape = binding_shape(*binding, error);
        if (!shape) {
            return false;
        }
        const std::string call = "static_cast<Self&>(*this)." + binding->region + "(" +
                                 (shape->event ? shape->param_name : std::string()) + ");";
        writer.line(shape->return_type + " " + binding->region + "(" +
                    (shape->event ? shape->param_type + " " + shape->param_name : std::string()) +
                    ") { return " + call + " }");
    }
    writer.line(
        "// Accessors for developer code: names come from the TSX, not the generated fields.");
    for (const ir::Prop& prop : component.props) {
        if (prop.type == "children" || !is_value_prop_type(prop.type, module.types)) {
            continue;
        }
        const std::optional<std::string> type = cpp_type(prop.type, module.types);
        if (!type || !claim(prop.name)) {
            return type.has_value() ? false
                                    : base_fail(error, "prop `" + prop.name + "` has no C++ type");
        }
        writer.line("[[nodiscard]] const " + *type + "& " + prop.name + "() const { return " +
                    member_name("prop::" + prop.name) + "; }");
    }
    for (const ir::State& state : component.state) {
        const std::optional<std::string> type = cpp_type(state.type, module.types);
        if (!type || !claim(state.name)) {
            return type.has_value()
                       ? false
                       : base_fail(error, "state `" + state.name + "` has no C++ type");
        }
        writer.line("[[nodiscard]] const " + *type + "& " + state.name + "() const { return " +
                    member_name(state.id) + "; }");
    }
    for (const ir::Ref& ref : component.refs) {
        if (!claim("ref_" + ref.name)) {
            return false;
        }
        writer.line("[[nodiscard]] tessera::ui::Node* ref_" + ref.name + "() const { return " +
                    member_name(ref.id) + "; }");
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        const std::optional<std::string> type = cpp_type(value.type, module.types);
        if (!type || !claim(value.name)) {
            return type.has_value()
                       ? false
                       : base_fail(error, "animated value `" + value.name + "` has no C++ type");
        }
        writer.line("[[nodiscard]] " + *type + " " + value.name + "() const { return " +
                    member_name(value.id) + "; }");
    }
    // A component instance the caller owns is a generated member the caller's developer code has to
    // reach to write the child's props (ADR-040 decision item 2 keeps the props public on the
    // child, so the only missing door is the instance itself). A per-item instance is owned by the
    // keyed item record instead of by a caller member, so it gets no accessor here.
    for (const ComponentInstance& instance : instances) {
        if (instance.in_template()) {
            continue;
        }
        const std::string name = "child_" + std::to_string(instance.ordinal);
        if (!claim(name)) {
            return false;
        }
        writer.line("// Component instance `" + instance.component_id +
                    "`: the caller's developer code writes the child's props through it.");
        writer.line("[[nodiscard]] " + instance.type_name + "& " + name + "() { return " +
                    instance_member(instance.ordinal) + "; }");
        writer.line("[[nodiscard]] const " + instance.type_name + "& " + name +
                    "() const { return " + instance_member(instance.ordinal) + "; }");
    }
    // ADR-050 section 3.1 (AF5): the `useService<T>()` accessor surface, emitted only for a
    // component that declares a requirement, so a component without one generates no service code.
    // The private slot tuple was already emitted in the header's field run by emit_service_fields.
    if (!emit_behavior_accessors(writer, component, error)) {
        return false;
    }
    return emit_service_accessors(writer, component, error);
}

} // namespace tessera::compiler::codegen::internal
