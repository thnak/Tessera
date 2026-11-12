// SPDX-License-Identifier: MIT

// Build-time library component IR writer (issue #70). `tessera_codegen --emit-library-cpp` lowers
// `tessera/components/*.tsx` through the ordinary front end and writes the C++ translation unit
// that rebuilds each component's IR. The TSX is the single source of truth; the generated data is
// compiled into `tessera_library_components` and is byte-for-byte reproducible.

#include "emit_library_components.hpp"
#include "emit_library_components_internal.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/node.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ir = tessera::compiler::ir;

using internal::expr_source;
using internal::is_project_relative;

// `std::quoted` (declared by <iomanip>, which the Android NDK pulls in transitively) is a better
// match for a std::string argument than a string_view overload, so an unqualified call would return
// a stream proxy instead of std::string. This non-template overload wins overload resolution over
// the std::quoted template and forwards to the internal escaper.
[[nodiscard]] std::string quoted(const std::string& text) {
    return internal::quoted(text);
}

// The stable identity band of the library components. The previous hand-built declarations used
// these numbers and the committed codegen goldens pin them, so generation keeps the same scheme:
// component N (in name order) is `...001 + N`, and its node with the pre-order id `node::K` is
// `...101 + 100 * N + K - 1`. A component may therefore have at most 100 nodes; a larger tree is
// a loud generation failure rather than a silent overlap with the next component.
constexpr std::uint64_t kComponentIdBase = 900000000000000001ULL;
constexpr std::uint64_t kNodeUiIdBase = 900000000000000101ULL;
constexpr std::uint64_t kNodeUiIdStride = 100ULL;
constexpr unsigned kNodesPerComponent = 100;

bool node_ordinal(const ir::Node& node, unsigned& ordinal, std::string& error) {
    constexpr std::string_view kPrefix = "node::";
    if (!node.id.starts_with(kPrefix)) {
        error = "node id `" + node.id + "` is not `node::N`";
        return false;
    }
    unsigned value = 0;
    const char* first = node.id.data() + kPrefix.size();
    const char* last = node.id.data() + node.id.size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || value == 0) {
        error = "node id `" + node.id + "` is not a positive `node::N` id";
        return false;
    }
    if (value > kNodesPerComponent) {
        error = "node id `" + node.id + "` is outside the 100-node library identity band";
        return false;
    }
    ordinal = value;
    return true;
}

void span_lines(std::string& out, std::string_view target, const ir::SourceSpan& span) {
    const std::string prefix = "    " + std::string(target) + ".";
    out += prefix + "file = " + quoted(span.file) + ";\n";
    out += prefix + "offset = " + std::to_string(span.offset) + ";\n";
    out += prefix + "length = " + std::to_string(span.length) + ";\n";
    out += prefix + "line = " + std::to_string(span.line) + ";\n";
    out += prefix + "column = " + std::to_string(span.column) + ";\n";
    out += prefix + "end_line = " + std::to_string(span.end_line) + ";\n";
    out += prefix + "end_column = " + std::to_string(span.end_column) + ";\n";
}

struct Emitter {
    std::string out;
    std::string error;
    std::size_t index = 0;
    std::set<unsigned> seen;
};

bool emit_node(Emitter& emitter, const ir::Node& node) {
    unsigned ordinal = 0;
    if (!node_ordinal(node, ordinal, emitter.error)) {
        return false;
    }
    if (!emitter.seen.insert(ordinal).second) {
        emitter.error = "duplicate node id `" + node.id + "` in one component";
        return false;
    }
    if (!is_project_relative(node.source.file)) {
        emitter.error = "node `" + node.id + "` has a non-project-relative source file `" +
                        node.source.file + "`";
        return false;
    }
    for (const ir::Node& child : node.children) {
        if (!emit_node(emitter, child)) {
            return false;
        }
    }
    if (node.fallback && !emit_node(emitter, *node.fallback)) {
        return false;
    }
    if (node.item_template && !emit_node(emitter, *node.item_template)) {
        return false;
    }

    const std::string function =
        "component_" + std::to_string(emitter.index) + "_node_" + std::to_string(ordinal);
    const std::string kind(ir::node_kind_name(node.kind));
    if (kind.empty()) {
        emitter.error = "node `" + node.id + "` has no IR kind name";
        return false;
    }
    std::string& out = emitter.out;
    out += "ir::Node " + function + "() {\n";
    out += "    ir::Node node;\n";
    out += "    node.kind = ir::NodeKind::" + kind + ";\n";
    out += "    node.id = " + quoted(node.id) + ";\n";
    const std::uint64_t ui_id = kNodeUiIdBase + kNodeUiIdStride * emitter.index + ordinal - 1;
    out += "    node.ui_id = \"" + std::to_string(ui_id) + "\";\n";
    if (!node.automation_id.empty()) {
        out += "    node.automation_id = " + quoted(node.automation_id) + ";\n";
    }
    span_lines(out, "node.source", node.source);
    if (!node.element.empty()) {
        out += "    node.element = " + quoted(node.element) + ";\n";
    }
    if (!node.component.empty()) {
        out += "    node.component = " + quoted(node.component) + ";\n";
    }
    if (node.key) {
        std::string value;
        if (!expr_source(*node.key, value, emitter.error)) {
            return false;
        }
        out += "    node.key = " + value + ";\n";
    }
    for (const auto& [name, value] : node.props) {
        std::string source;
        if (!value || !expr_source(*value, source, emitter.error)) {
            emitter.error = "prop `" + name + "` of `" + node.id + "` has no expression";
            return false;
        }
        out += "    node.props.emplace(" + quoted(name) + ", " + source + ");\n";
    }
    for (const auto& [name, value] : node.style) {
        std::string source;
        if (!value || !expr_source(*value, source, emitter.error)) {
            emitter.error = "style `" + name + "` of `" + node.id + "` has no expression";
            return false;
        }
        out += "    node.style.emplace(" + quoted(name) + ", " + source + ");\n";
    }
    for (const auto& [name, id] : node.events) {
        out += "    node.events.emplace(" + quoted(name) + ", " + quoted(id) + ");\n";
    }
    if (node.text) {
        std::string value;
        if (!expr_source(*node.text, value, emitter.error)) {
            return false;
        }
        out += "    node.text = " + value + ";\n";
    }
    if (node.condition) {
        std::string value;
        if (!expr_source(*node.condition, value, emitter.error)) {
            return false;
        }
        out += "    node.condition = " + value + ";\n";
    }
    for (const ir::Node& child : node.children) {
        unsigned child_ordinal = 0;
        if (!node_ordinal(child, child_ordinal, emitter.error)) {
            return false;
        }
        out += "    node.children.push_back(component_" + std::to_string(emitter.index) + "_node_" +
               std::to_string(child_ordinal) + "());\n";
    }
    if (node.fallback) {
        unsigned fallback_ordinal = 0;
        if (!node_ordinal(*node.fallback, fallback_ordinal, emitter.error)) {
            return false;
        }
        out += "    node.fallback = std::make_unique<ir::Node>(component_" +
               std::to_string(emitter.index) + "_node_" + std::to_string(fallback_ordinal) +
               "());\n";
    }
    if (node.item_template) {
        unsigned template_ordinal = 0;
        if (!node_ordinal(*node.item_template, template_ordinal, emitter.error)) {
            return false;
        }
        out += "    node.item_template = std::make_unique<ir::Node>(component_" +
               std::to_string(emitter.index) + "_node_" + std::to_string(template_ordinal) +
               "());\n";
    }
    if (node.loop) {
        if (!node.loop->source) {
            emitter.error = "loop of `" + node.id + "` has no source expression";
            return false;
        }
        std::string source;
        if (!expr_source(*node.loop->source, source, emitter.error)) {
            return false;
        }
        out += "    node.loop.emplace();\n";
        out += "    node.loop->item = " + quoted(node.loop->item) + ";\n";
        if (!node.loop->index.empty()) {
            out += "    node.loop->index = " + quoted(node.loop->index) + ";\n";
        }
        out += "    node.loop->source = " + source + ";\n";
    }
    out += "    return node;\n";
    out += "}\n\n";
    return true;
}

bool presentational(const ir::Component& component, std::string& error) {
    if (!component.state.empty() || !component.handlers.empty() || !component.effects.empty() ||
        !component.refs.empty() || !component.animated_values.empty() ||
        !component.environment.empty()) {
        error = "component `" + component.name +
                "` is not presentational: a library component may declare props and a node tree "
                "only";
        return false;
    }
    if (component.name.empty() || !component.id.starts_with("comp::")) {
        error = "library component has no `comp::<Name>` id";
        return false;
    }
    if (!is_project_relative(component.source.file)) {
        error = "component `" + component.name + "` has a non-project-relative source file `" +
                component.source.file + "`";
        return false;
    }
    return true;
}

bool emit_component(Emitter& emitter, const ir::Component& component) {
    std::string& out = emitter.out;
    out += "ir::Component make_component_" + std::to_string(emitter.index) + "() {\n";
    out += "    ir::Component component;\n";
    out += "    component.id = " + quoted(component.id) + ";\n";
    out += "    component.component_id = \"" + std::to_string(kComponentIdBase + emitter.index) +
           "\";\n";
    out += "    component.name = " + quoted(component.name) + ";\n";
    span_lines(out, "component.source", component.source);
    for (const ir::Prop& prop : component.props) {
        out += "    {\n";
        out += "        ir::Prop prop;\n";
        out += "        prop.name = " + quoted(prop.name) + ";\n";
        out += "        prop.type = " + quoted(prop.type) + ";\n";
        if (prop.optional) {
            out += "        prop.optional = true;\n";
        }
        if (prop.default_value) {
            std::string value;
            if (!expr_source(*prop.default_value, value, emitter.error)) {
                return false;
            }
            out += "        prop.default_value = " + value + ";\n";
        }
        out += "        component.props.push_back(std::move(prop));\n";
        out += "    }\n";
    }
    unsigned root_ordinal = 0;
    if (!node_ordinal(component.root, root_ordinal, emitter.error)) {
        return false;
    }
    out += "    component.root = component_" + std::to_string(emitter.index) + "_node_" +
           std::to_string(root_ordinal) + "();\n";
    out += "    return component;\n";
    out += "}\n\n";
    return true;
}

} // namespace

bool emit_library_components_cpp(const ir::IrModule& module, const std::string& path,
                                 std::string& error) {
    if (module.components.empty()) {
        error = "the library module declares no components";
        return false;
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
            error = "duplicate library component name `" + components[i]->name + "`";
            return false;
        }
    }

    std::set<std::string> files;
    std::string out;
    out += "// SPDX-License-Identifier: MIT\n";
    out += "// clang-format off\n\n";
    out += "// Generated by `tessera_codegen --emit-library-cpp` from the library TSX sources.\n";
    out +=
        "// Do not edit: edit the TSX and rebuild, which regenerates this file and fails if the\n";
    out += "// sources do not lower cleanly.\n";
    out += "//\n";
    out += "// Sources:\n";
    for (const ir::Component* component : components) {
        files.insert(std::string(component->source.file));
    }
    for (const std::string& file : files) {
        out += "//   " + file + "\n";
    }
    out += "\n";
    out += "#include \"library_components_generated.hpp\"\n\n";
    out += "#include <array>\n";
    out += "#include <cstdint>\n";
    out += "#include <memory>\n";
    out += "#include <span>\n";
    out += "#include <string>\n";
    out += "#include <string_view>\n";
    out += "#include <utility>\n";
    out += "#include <vector>\n\n";
    out += "namespace tessera::compiler::lowering::generated {\n";
    out += "namespace {\n\n";

    Emitter emitter{};
    for (std::size_t index = 0; index < components.size(); ++index) {
        const ir::Component& component = *components[index];
        if (!presentational(component, error)) {
            return false;
        }
        emitter.index = index;
        emitter.seen.clear();
        if (!emit_node(emitter, component.root)) {
            return false;
        }
        if (!emit_component(emitter, component)) {
            return false;
        }
    }
    out += emitter.out;

    out += "constexpr std::array<LibraryComponent, " + std::to_string(components.size()) +
           "> kLibraryComponents{{\n";
    for (const ir::Component* component : components) {
        out +=
            "    {.name = " + quoted(component->name) + ", .id = " + quoted(component->id) + "},\n";
    }
    out += "}};\n\n";
    out += "} // namespace\n\n";
    out += "std::span<const LibraryComponent> library_components() {\n";
    out += "    return kLibraryComponents;\n";
    out += "}\n\n";
    out += "ir::Component make_library_component(const LibraryComponent& component) {\n";
    for (std::size_t index = 0; index < components.size(); ++index) {
        out += "    if (component.name == " + quoted(components[index]->name) + ") {\n";
        out += "        return make_component_" + std::to_string(index) + "();\n";
        out += "    }\n";
    }
    out +=
        "    // Unreachable through library_components(); keep a total answer for the encoding.\n";
    out += "    ir::Component empty;\n";
    out += "    empty.id = std::string(component.id);\n";
    out += "    empty.name = std::string(component.name);\n";
    out += "    return empty;\n";
    out += "}\n\n";
    out += "} // namespace tessera::compiler::lowering::generated\n";
    out += "// clang-format on\n";

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot write `" + path + "`";
        return false;
    }
    stream << out;
    if (!stream) {
        error = "cannot write `" + path + "`";
        return false;
    }
    return true;
}

} // namespace tessera::compiler::codegen::cli
