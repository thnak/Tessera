// SPDX-License-Identifier: MIT

// ADR-040 generated base (docs/design/compiler.md section 7). The legacy emitters write a header
// with a plain struct and a source with out-of-class definitions. A CRTP template has to define
// everything in its header, so this file reshapes those two texts instead of forking the dozens of
// emitters:
//   - `struct X` becomes `template <class Self> struct XBase`, definitions follow the struct as
//     `template <class Self> inline ... XBase<Self>::name(...)`;
//   - helper functions of the source (anonymous namespaces) move into `namespace X_detail`, marked
//     `inline`, and those that name the component become function templates over `Self` (the
//     event/scheduler thunks cast their `void*` context to `XBase<Self>*`);
//   - each `bind::<id>` is a same-named dispatch thunk calling the member of `Self`, and `build()`
//     starts with one static_assert per binding that names the id, the signature and the TSX
//     location.
// The writer output is deterministic (two-space indent, LF), so the reshaping is too.

#include "emit_base.hpp"
#include "emit_events.hpp"
#include "emit_props.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

using Lines = std::vector<std::string>;

Lines split_lines(const std::string& text) {
    Lines lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::size_t indent_of(std::string_view line) {
    std::size_t count = 0;
    while (count < line.size() && line[count] == ' ') {
        ++count;
    }
    return count;
}

std::string_view trimmed(std::string_view line) {
    line.remove_prefix(indent_of(line));
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Net `{` minus `}` of one line, skipping string/char literals and a trailing `//` comment.
int brace_delta(std::string_view line) {
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

// One top-level item of a body: leading comment lines plus the code lines of one declaration.
struct Item {
    Lines lead;
    Lines code;
};

std::vector<Item> group_items(const Lines& lines) {
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

bool contains_token(std::string_view text, std::string_view token) {
    for (std::size_t at = text.find(token); at != std::string_view::npos;
         at = text.find(token, at + 1)) {
        const bool left = at == 0 || !is_word(text[at - 1]);
        const bool right = at + token.size() >= text.size() || !is_word(text[at + token.size()]);
        if (left && right) {
            return true;
        }
    }
    return false;
}

// Replaces each whole-word `token` for which `accept(text, at)` holds.
template<class Accept>
std::string replace_token(const std::string& text, std::string_view token,
                          std::string_view replacement, Accept accept) {
    if (token.empty()) {
        return text; // find("") matches everywhere and never advances
    }
    std::string out;
    std::size_t last = 0;
    for (std::size_t at = text.find(token); at != std::string::npos;
         at = text.find(token, at + token.size())) {
        const bool left = at == 0 || !is_word(text[at - 1]);
        const std::size_t after = at + token.size();
        const bool right = after >= text.size() || !is_word(text[after]);
        if (left && right && accept(text, at)) {
            out.append(text, last, at - last);
            out.append(replacement);
            last = after;
        }
    }
    out.append(text, last, std::string::npos);
    return out;
}

// The component used as a type inside a body: `Name*`, `Name&`, `<Name>`; never a string literal,
// a qualified name or a field that merely shares the spelling.
bool used_as_type(const std::string& text, std::size_t at, std::size_t size) {
    if (at > 0 && (text[at - 1] == '"' || text[at - 1] == ':')) {
        return false;
    }
    std::size_t next = at + size;
    while (next < text.size() && text[next] == ' ') {
        ++next;
    }
    return next < text.size() && (text[next] == '*' || text[next] == '&' || text[next] == '>');
}

std::string with_class_type(const std::string& line, const std::string& name,
                            const std::string& base) {
    return replace_token(line, name, base + "<Self>", [&](const std::string& text, std::size_t at) {
        return used_as_type(text, at, name.size());
    });
}

// `[[attr]] int f()` -> `[[attr]] inline int f()`: the attribute list must stay first.
std::string with_inline(const std::string& line) {
    std::size_t at = indent_of(line);
    while (line.compare(at, 2, "[[") == 0) {
        const std::size_t close = line.find("]]", at);
        if (close == std::string::npos) {
            break;
        }
        at = close + 2;
        while (at < line.size() && line[at] == ' ') {
            ++at;
        }
    }
    return line.substr(0, at) + "inline " + line.substr(at);
}

std::string escape_message(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

std::string function_name(const std::string& line) {
    const std::size_t paren = line.find('(');
    if (paren == std::string::npos) {
        return {};
    }
    std::size_t begin = paren;
    while (begin > 0 && is_word(line[begin - 1])) {
        --begin;
    }
    return line.substr(begin, paren - begin);
}

void append(Lines& out, const Lines& lines, std::size_t extra_indent = 0) {
    for (const std::string& line : lines) {
        out.push_back(line.empty() ? line : std::string(extra_indent, ' ') + line);
    }
}

std::string with_static(const std::string& line) {
    const std::size_t at = indent_of(line);
    return line.substr(0, at) + "static " + line.substr(at);
}

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

// ADR-040 item 2: generated fields and the dispatch thunks are private. Methods stay public (the
// host drives build/mount/update/..., the developer reads accessors), and so do the `prop_` fields:
// a parent component writes the props of the child instance it owns.
Lines apply_access(const Lines& struct_body, const std::string& friend_line) {
    Lines out;
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
            wants_private = !starts_with(data_member_name(item), "prop_");
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
    static constexpr std::string_view kReserved[] = {
        "build",           "mount",           "update",
        "unmount",         "apply_state",     "update_effects",
        "snapshot_state",  "schedule_update", "attach_scheduler",
        "detach_scheduler"};
    return std::find(std::begin(kReserved), std::end(kReserved), name) != std::end(kReserved);
}

} // namespace

std::optional<BindingShape> binding_shape(const ir::Binding& binding, std::string& error) {
    BindingShape shape;
    shape.return_type = "void";
    if (binding.kind == ir::BindingKind::Event) {
        bool typed = false;
        std::string param_name;
        const std::optional<std::string> signature =
            payload_signature(binding.params, typed, param_name);
        if (!signature) {
            fail(error, "binding `" + binding.id + "` has an unsupported payload record");
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
            fail(error, "binding `" + binding.id + "` has an unsupported return type `" +
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

bool emit_base_members(CppWriter& writer, const ir::Component& component,
                       const std::vector<const ir::Binding*>& bindings, std::string& error) {
    std::set<std::string> names;
    const auto claim = [&](const std::string& name) {
        if (reserved_member(name) || !names.insert(name).second) {
            return fail(error, "component `" + component.name + "`: accessor `" + name +
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
        if (prop.type == "children" || !is_value_prop_type(prop.type)) {
            continue;
        }
        const std::optional<std::string> type = cpp_type(prop.type);
        if (!type || !claim(prop.name)) {
            return type.has_value() ? false
                                    : fail(error, "prop `" + prop.name + "` has no C++ type");
        }
        writer.line("[[nodiscard]] const " + *type + "& " + prop.name + "() const { return " +
                    member_name("prop::" + prop.name) + "; }");
    }
    for (const ir::State& state : component.state) {
        const std::optional<std::string> type = cpp_type(state.type);
        if (!type || !claim(state.name)) {
            return type.has_value() ? false
                                    : fail(error, "state `" + state.name + "` has no C++ type");
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
        const std::optional<std::string> type = cpp_type(value.type);
        if (!type || !claim(value.name)) {
            return type.has_value()
                       ? false
                       : fail(error, "animated value `" + value.name + "` has no C++ type");
        }
        writer.line("[[nodiscard]] " + *type + " " + value.name + "() const { return " +
                    member_name(value.id) + "; }");
    }
    return true;
}

bool emit_generated_base(const ir::Component& component,
                         const std::vector<const ir::Binding*>& bindings,
                         const std::string& legacy_header, const std::string& legacy_source,
                         GeneratedFile& out, std::string& error) {
    const std::string& name = component.name;
    const std::string base = name + "Base";
    const std::string detail = name + "_detail";

    // ---- legacy header: preamble, comment, auxiliary declarations, struct body
    const Lines header = split_lines(legacy_header);
    std::size_t ns_open = 0;
    while (ns_open < header.size() && !starts_with(header[ns_open], "namespace ")) {
        ++ns_open;
    }
    if (ns_open + 4 >= header.size() || header[ns_open + 1] != "  namespace gen {") {
        return fail(error, "the generated header of `" + name + "` has an unexpected shape");
    }
    const std::string module_open = header[ns_open];
    Lines body(header.begin() + static_cast<std::ptrdiff_t>(ns_open) + 2, header.end() - 2);
    const std::string struct_line = "    struct " + name + " {";
    std::size_t struct_at = 0;
    while (struct_at < body.size() && body[struct_at] != struct_line) {
        ++struct_at;
    }
    std::size_t struct_end = body.size();
    while (struct_end > 0 && body[struct_end - 1] != "    };") {
        --struct_end;
    }
    if (struct_at >= body.size() || struct_end == 0 || struct_end - 1 <= struct_at) {
        return fail(error, "the generated header of `" + name + "` has no component struct");
    }
    --struct_end;
    const std::string comment = body.empty() ? std::string() : body.front();
    Lines aux;
    for (std::size_t i = 1; i < struct_at; ++i) {
        aux.push_back(body[i]);
    }
    Lines struct_body(body.begin() + static_cast<std::ptrdiff_t>(struct_at) + 1,
                      body.begin() + static_cast<std::ptrdiff_t>(struct_end));
    std::vector<std::string> aux_names;
    for (const std::string& line : aux) {
        const std::string_view text = trimmed(line);
        if (starts_with(text, "struct ")) {
            const std::string_view rest = text.substr(7);
            aux_names.emplace_back(rest.substr(0, rest.find(' ')));
        } else if (starts_with(text, "inline constexpr ")) {
            const std::size_t eq = text.find(" =");
            if (eq != std::string_view::npos) {
                std::size_t begin = eq;
                while (begin > 0 && is_word(text[begin - 1])) {
                    --begin;
                }
                aux_names.emplace_back(text.substr(begin, eq - begin));
            }
        }
    }
    const bool has_aux = std::any_of(
        aux.begin(), aux.end(), [](const std::string& line) { return !trimmed(line).empty(); });
    const auto qualify_aux = [&](std::string line) {
        for (const std::string& aux_name : aux_names) {
            line = replace_token(line, aux_name, detail + "::" + aux_name,
                                 [](const std::string& text, std::size_t at) {
                                     return at < 2 || text.compare(at - 2, 2, "::") != 0;
                                 });
        }
        return line;
    };

    // ---- legacy source: includes, helper namespaces, member definitions
    const Lines source = split_lines(legacy_source);
    Lines extra_includes;
    std::size_t gen_open = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (starts_with(source[i], "#include ") && source[i] != "#include \"" + name + ".hpp\"") {
            extra_includes.push_back(source[i]);
        }
        if (source[i] == "  namespace gen {") {
            gen_open = i;
            break;
        }
    }
    if (gen_open == 0 || source.size() < gen_open + 3) {
        return fail(error, "the generated source of `" + name + "` has an unexpected shape");
    }
    const Lines source_body(source.begin() + static_cast<std::ptrdiff_t>(gen_open) + 1,
                            source.end() - 2);
    std::vector<Item> detail_items;
    std::vector<Item> members;
    for (Item& item : group_items(source_body)) {
        const std::string_view first = trimmed(item.code.front());
        if (first == "namespace {") {
            Lines inner(item.code.begin() + 1, item.code.end() - 1);
            for (Item& inner_item : group_items(inner)) {
                detail_items.push_back(std::move(inner_item));
            }
            continue;
        }
        const std::string& line = item.code.front();
        const std::size_t paren = line.find('(');
        const bool member =
            line.find(name + "::") != std::string::npos && line.find(name + "::") < paren;
        if (member) {
            members.push_back(std::move(item));
            continue;
        }
        // A free function of the gen namespace: re-indent into the detail namespace.
        for (std::string& code_line : item.code) {
            code_line = "  " + code_line;
        }
        for (std::string& lead_line : item.lead) {
            lead_line = "  " + lead_line;
        }
        detail_items.push_back(std::move(item));
    }

    // Detail helpers: component-typed ones become templates, the rest are `inline`.
    std::set<std::string> thunks;
    std::vector<Item> thunk_items;
    std::vector<Item> free_items;
    for (Item& item : detail_items) {
        const std::string_view first = trimmed(item.code.front());
        if (starts_with(first, "constexpr") || starts_with(first, "struct") ||
            starts_with(first, "using") || starts_with(first, "enum") ||
            starts_with(first, "inline constexpr")) {
            free_items.push_back(std::move(item));
            continue;
        }
        bool mentions = false;
        for (const std::string& line : item.code) {
            mentions = mentions || contains_token(line, name);
        }
        if (mentions) {
            if (const std::string thunk = function_name(item.code.front()); !thunk.empty()) {
                thunks.insert(thunk);
            }
            for (std::string& line : item.code) {
                line = "  " + with_class_type(line, name, base);
            }
            for (std::string& line : item.lead) {
                line = "  " + line;
            }
            item.code.front() = with_static(item.code.front());
            thunk_items.push_back(std::move(item));
        } else {
            item.code.front() = with_inline(item.code.front());
            free_items.push_back(std::move(item));
        }
    }
    detail_items = std::move(free_items);
    const bool has_detail = !detail_items.empty() || !thunk_items.empty() || has_aux;

    // Contract: one static_assert per binding, first thing build() does.
    Lines contract;
    Lines concept_lines;
    concept_lines.push_back("    template <class Self>");
    if (bindings.empty()) {
        concept_lines.push_back("    concept " + name + "Contract = true;");
    } else {
        concept_lines.push_back("    concept " + name + "Contract = requires {");
    }
    for (const ir::Binding* binding : bindings) {
        const std::optional<BindingShape> shape = binding_shape(*binding, error);
        if (!shape) {
            return false;
        }
        const std::string member = binding->region;
        const std::string pointer = shape->return_type + " (Self::*)(" +
                                    (shape->event ? shape->param_type : std::string()) + ")";
        const std::string own = "!std::is_same_v<decltype(&Self::" + member + "), decltype(&" +
                                base + "<Self>::" + member + ")>";
        const std::string fits =
            "std::is_convertible_v<decltype(&Self::" + member + "), " + pointer + ">";
        const std::string message = "TESSERA_CG_MISSING_BINDING: component `" + name + "` needs `" +
                                    binding_signature(*binding, *shape) + "` for " + binding->id +
                                    " (" + binding->source.file + ":" +
                                    std::to_string(binding->source.line) + ":" +
                                    std::to_string(binding->source.column) +
                                    "); define it in the developer class (ADR-040)";
        contract.push_back("      static_assert(" + own + " &&");
        contract.push_back("                        " + fits + ",");
        contract.push_back("                    \"" + escape_message(message) + "\");");
        // The thunk is private, so outside the base `&Self::member` of a class that does not define
        // `member` names an inaccessible base member: the requirement is simply false. (The
        // authoritative check is the static_assert in build(), which runs inside the friend.)
        concept_lines.push_back("      requires " + fits + ";");
    }
    if (!bindings.empty()) {
        concept_lines.push_back("    };");
    }

    // ---- assemble
    Lines out_lines;
    std::size_t last_include = 0;
    for (std::size_t i = 0; i < ns_open; ++i) {
        out_lines.push_back(header[i]);
        if (starts_with(header[i], "#include ")) {
            last_include = out_lines.size();
        }
    }
    out_lines[2] = "// ADR-040 generated base: do not edit. Developer code goes in " + name +
                   ".hpp/.cpp, derived from " + base + "<Self>.";
    Lines includes;
    for (const std::string& include : extra_includes) {
        if (std::find(out_lines.begin(), out_lines.end(), include) == out_lines.end()) {
            includes.push_back(include);
        }
    }
    if (std::find(out_lines.begin(), out_lines.end(), "#include <type_traits>") ==
        out_lines.end()) {
        includes.push_back("#include <type_traits>");
    }
    out_lines.insert(out_lines.begin() + static_cast<std::ptrdiff_t>(last_include),
                     includes.begin(), includes.end());
    out_lines.push_back(module_open);
    out_lines.push_back("  namespace gen {");
    out_lines.push_back(comment);
    out_lines.push_back("    template <class Self>");
    out_lines.push_back("    struct " + base + ";");
    if (!thunk_items.empty()) {
        out_lines.push_back("    namespace " + detail + " {");
        out_lines.push_back("      template <class Self>");
        out_lines.push_back("      struct Thunks;");
        out_lines.push_back("    }");
    }
    if (has_aux) {
        out_lines.push_back("    namespace " + detail + " {");
        append(out_lines, aux, 2);
        out_lines.push_back("    }");
    }
    out_lines.push_back("    template <class Self>");
    out_lines.push_back("    struct " + base + " {");
    const std::string friend_line =
        thunk_items.empty() ? std::string() : "      friend struct " + detail + "::Thunks<Self>;";
    for (const std::string& line : apply_access(struct_body, friend_line)) {
        out_lines.push_back(qualify_aux(line));
    }
    out_lines.push_back("    };");
    append(out_lines, concept_lines);
    if (!detail_items.empty() || !thunk_items.empty()) {
        out_lines.push_back("    namespace " + detail + " {");
        for (const Item& item : detail_items) {
            append(out_lines, item.lead);
            append(out_lines, item.code);
        }
        if (!thunk_items.empty()) {
            out_lines.push_back("      template <class Self>");
            out_lines.push_back("      struct Thunks {");
            for (const Item& item : thunk_items) {
                append(out_lines, item.lead);
                append(out_lines, item.code);
            }
            out_lines.push_back("      };");
        }
        out_lines.push_back("    }");
    }
    for (Item& item : members) {
        append(out_lines, item.lead);
        std::string& head = item.code.front();
        head.replace(head.find(name + "::"), name.size() + 2, base + "<Self>::");
        out_lines.push_back("    template <class Self>");
        item.code.front() = with_inline(item.code.front());
        const bool is_build = head.find(base + "<Self>::build(") != std::string::npos;
        int depth = 0;
        bool opened = false;
        for (std::string& line : item.code) {
            std::string text = qualify_aux(with_class_type(line, name, base));
            for (const std::string& thunk : thunks) {
                text = replace_token(text, thunk, "Thunks<Self>::" + thunk,
                                     [&](const std::string& whole, std::size_t at) {
                                         // `&thunk` (an adapter address) or `thunk(` (a helper
                                         // call), never a member access of the same spelling.
                                         if (at > 0 && whole[at - 1] == '&') {
                                             return true;
                                         }
                                         const std::size_t next = at + thunk.size();
                                         return next < whole.size() && whole[next] == '(' &&
                                                (at == 0 ||
                                                 (whole[at - 1] != '.' && whole[at - 1] != '>'));
                                     });
            }
            out_lines.push_back(text);
            depth += brace_delta(line);
            if (!opened && depth > 0) {
                opened = true;
                if (has_detail) {
                    out_lines.push_back("      using namespace " + detail + ";");
                }
                if (is_build) {
                    append(out_lines, contract);
                }
            }
        }
    }
    out_lines.push_back("  }");
    out_lines.push_back("}");

    out.path = name + ".g.hpp";
    out.text.clear();
    for (const std::string& line : out_lines) {
        out.text += line;
        out.text += '\n';
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
