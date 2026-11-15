// SPDX-License-Identifier: MIT

// ADR-040 generated base, access section: the generated data members are private and the
// developer class reads them through generated accessors. This TU owns the member-shape helpers
// (`is_data_member`, `data_member_name`), the `private:`/`public:` partitioning of the struct body
// (`apply_access`) and the accessor emission itself (`emit_base_members`), so `emit_base.cpp` stays
// a text-reshaping file under the 500-line hard limit (AGENTS.md, Code conventions).
//
// The split is behavior-preserving: the accessors are emitted in the same order with the same text
// as before, which the committed `.g.hpp` goldens prove.

#pragma once

#include "codegen_internal.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {

// ---- Shared text primitives of the generated-base reshaping -------------------------------
// Both `emit_base.cpp` (the reshaping itself) and `emit_base_accessors.cpp` (member shape and
// accessors) walk the same writer text line by line, so these live here instead of in either TU.

using Lines = std::vector<std::string>;

inline bool base_fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

inline std::size_t indent_of(std::string_view line) {
    std::size_t count = 0;
    while (count < line.size() && line[count] == ' ') {
        ++count;
    }
    return count;
}

inline std::string_view trimmed(std::string_view line) {
    line.remove_prefix(indent_of(line));
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

inline bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

inline bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Net `{` minus `}` of one line, skipping string/char literals and a trailing `//` comment.
inline int brace_delta(std::string_view line) {
    int delta = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            break;
        }
        if (c == '"' || c == '\'') {
            for (++i; i < line.size() && line[i] != c; ++i) {
                if (line[i] == '\\') {
                    ++i;
                }
            }
            continue;
        }
        delta += (c == '{') - (c == '}');
    }
    return delta;
}

inline void append(Lines& out, const Lines& lines, std::size_t extra_indent = 0) {
    for (const std::string& line : lines) {
        out.push_back(line.empty() ? line : std::string(extra_indent, ' ') + line);
    }
}

// The C++ shape of the member a `bind::` id requires on the developer class.
struct BindingShape {
    std::string return_type; // `void` when the binding returns nothing
    std::string param_type;  // empty when the member takes no argument
    std::string param_name;  // the generated parameter name, empty with no argument
    bool event = false;
};

// Null with `error` set when the binding names a payload or return type with no C++ form.
[[nodiscard]] std::optional<BindingShape> binding_shape(const ir::Binding& binding,
                                                        std::string& error);

// `void sync()` / `void on_click(const tessera::ui::ClickEvent& event)`: the declaration text a
// developer writes (the base's diagnostics print it).
[[nodiscard]] std::string binding_signature(const ir::Binding& binding, const BindingShape& shape);

// One top-level item of a body: leading comment lines plus the code lines of one declaration.
struct Item {
    std::vector<std::string> lead;
    std::vector<std::string> code;
};

// Groups the declarations of a struct body into items, keeping each declaration's leading comments
// with it. A declaration ends when its braces balance, or at its `;` when it has no brace.
[[nodiscard]] std::vector<Item> group_items(const std::vector<std::string>& lines);

// The `private:`/`public:` section transitions of the generated struct body, prefixed with the
// friend declaration when the component has one (ADR-040 item 2).
[[nodiscard]] std::vector<std::string> apply_access(const std::vector<std::string>& struct_body,
                                                    const std::string& friend_line);

// True when a generated member name is already used by the base's own API (`build`, `mount`, ...),
// so an accessor of that name would collide.
[[nodiscard]] bool reserved_member(std::string_view name);

// Emits the in-class dispatch thunks (one per binding, calling the same-named member of `Self`),
// the accessors (`x()` per state, `ref_<name>()` per ref, `<name>()` per prop and animated value,
// `child_<n>()` per non-template component instance) and the developer-facing contract. Fails on an
// accessor name that collides with a generated member.
[[nodiscard]] bool emit_base_members(CppWriter& writer, const ir::IrModule& module,
                                     const ir::Component& component,
                                     const std::vector<const ir::Binding*>& bindings,
                                     const std::vector<ComponentInstance>& instances,
                                     std::string& error);

} // namespace tessera::compiler::codegen::internal
