// SPDX-License-Identifier: MIT

// The `inline` destination of `source.patch` (issue #171, designer-protocol-v1 section 10.1): the
// `style` attribute of the JSX element the committed node came from.
//
// **Why the range comes from the parser and not from a search.** The IR records the element's span
// but not its attributes', so this re-parses the component's file and finds the element whose span
// starts where the node's does. The `JsxAttribute::span` the parser recorded is then the exact byte
// range of the attribute, including its name and value - which is what an edit must replace. A
// `find("style=")` over the text would match a string literal, a comment or a nested element, and
// the first time it did, a commit would rewrite the wrong bytes.
//
// **Why the element is matched by offset.** `ir::Node::source` is the element's span, and the
// lowering copies it verbatim from the AST (`node_lowering.cpp`), so the two offsets are the same
// number. Matching on it is exact; matching on the tag name or the id would not be, because a
// component can contain two `<View>`s.

#include "scene_canvas_patch_internal.hpp"
#include "tessera/compiler/jsx/parser.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli::internal {
namespace {

namespace jsx = tessera::compiler::jsx;

// The element of `node`'s subtree whose span starts at `offset`, or null. Depth-first, so the first
// match in source order wins; two elements cannot start at the same byte.
const jsx::JsxNode* element_at(const jsx::JsxNode& node, std::uint32_t offset) {
    if (node.kind == jsx::JsxKind::Element && node.span.offset == offset) {
        return &node;
    }
    for (const jsx::JsxPtr& child : node.children) {
        if (child != nullptr) {
            if (const jsx::JsxNode* found = element_at(*child, offset)) {
                return found;
            }
        }
    }
    return nullptr;
}

// The same walk over every component of the module: the node's span names the file it is in, and
// the element is somewhere in that file's returned tree.
const jsx::JsxNode* element_in_module(const jsx::Module& module, std::uint32_t offset) {
    for (const jsx::ModuleItem& item : module.items) {
        if (item.kind != jsx::ItemKind::Component || item.returned == nullptr) {
            continue;
        }
        if (const jsx::JsxNode* found = element_at(*item.returned, offset)) {
            return found;
        }
    }
    return nullptr;
}

// The attribute named `name`, or null.
const jsx::JsxAttribute* find_attribute(const jsx::JsxNode& element, std::string_view name) {
    for (const jsx::JsxAttribute& attribute : element.attributes) {
        if (attribute.name == name) {
            return &attribute;
        }
    }
    return nullptr;
}

// One declaration of the rewritten attribute: either the source text the developer already wrote
// (preserved byte for byte) or a value the commit supplies.
struct MergedDeclaration {
    std::string name;  // kebab-case, as the override layer spells it
    std::string value; // the JSX value text, already quoted or numeric
};

// A kebab-case CSS property as the JSX subset spells it: `padding-left` -> `paddingLeft`.
std::string camel_case(std::string_view property) {
    std::string out;
    out.reserve(property.size());
    bool upper = false;
    for (const char c : property) {
        if (c == '-') {
            upper = true;
            continue;
        }
        out.push_back(upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        upper = false;
    }
    return out;
}

// A CSS value as a JSX attribute value. A bare number stays a number (the subset reads a number as
// `<n>px` for a length property and unitless otherwise, which is what the developer wrote); every
// other value is a double-quoted string, with the quote and backslash escaped so the attribute
// cannot be broken by the value it carries.
std::string jsx_value(std::string_view value) {
    const bool numeric = !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E';
    });
    if (numeric) {
        return std::string(value);
    }
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// The `style` attribute's text for `declarations`, in the order the override layer holds them.
//
// **Why the existing declarations are kept, byte for byte.** A commit writes the node's overrides,
// not the whole inline style: an attribute that already sets `color` and gains an override for
// `padding` must end up with both, and the `color` it keeps must be the text the developer wrote.
// Dropping or reformatting the ones the commit did not name would silently rewrite a value nobody
// asked to change.
std::string style_attribute_text(std::string_view source, const jsx::JsxNode& element,
                                 const std::vector<Declaration>& declarations) {
    std::vector<MergedDeclaration> merged;
    const jsx::JsxAttribute* existing = find_attribute(element, "style");
    if (existing != nullptr && existing->kind == jsx::JsxAttrKind::Expression &&
        existing->expr != nullptr && existing->expr->kind == jsx::ExprKind::Object) {
        for (const jsx::ObjectProperty& property : existing->expr->properties) {
            if (property.value == nullptr) {
                continue;
            }
            const jsx::Span span = property.value->span;
            const std::string_view raw = span.offset + span.length <= source.size()
                                             ? source.substr(span.offset, span.length)
                                             : std::string_view();
            merged.push_back(MergedDeclaration{property.key, std::string(raw)});
        }
    }
    for (const Declaration& declaration : declarations) {
        const auto found =
            std::find_if(merged.begin(), merged.end(), [&](const MergedDeclaration& entry) {
                return entry.name == declaration.first;
            });
        if (found != merged.end()) {
            found->value = jsx_value(declaration.second);
        } else {
            merged.push_back(MergedDeclaration{declaration.first, jsx_value(declaration.second)});
        }
    }
    // The house spelling of an inline style (`samples/compiler-demo/Demo.tsx` and the primitives
    // both write it this way): `style={{ a: 1, b: 2 }}`. Matching it keeps a committed attribute
    // looking like the one a developer would have typed.
    std::string text = "style={{ ";
    for (std::size_t index = 0; index < merged.size(); ++index) {
        if (index != 0) {
            text += ", ";
        }
        text += camel_case(merged[index].name);
        text += ": ";
        text += merged[index].value;
    }
    text += " }}";
    return text;
}

} // namespace

std::expected<std::vector<TextEdit>, PatchError> inline_edits(const PatchRequest& request,
                                                              const PatchContext& context) {
    if (context.file.empty() || context.span_length == 0) {
        return invalid_range("the node has no source span to edit");
    }
    const std::string_view text = project_text(context.project(), context.file);
    if (text.empty()) {
        return std::unexpected(PatchError{PatchErrorCode::StaleSource,
                                          "the project has no file `" + context.file + "`"});
    }
    if (!is_code_point_boundary(text, context.span_offset) ||
        !is_code_point_boundary(text, context.span_offset + context.span_length)) {
        return invalid_range("the node's span is not on a code point boundary");
    }
    const jsx::ModuleParseResult parsed = jsx::parse_module(text);
    if (!parsed.ok()) {
        return invalid_range("`" + context.file +
                             "` does not parse, so its attributes cannot be located");
    }
    const jsx::JsxNode* element = element_in_module(*parsed.root, context.span_offset);
    if (element == nullptr) {
        return invalid_range("no element of `" + context.file + "` starts at the node's span");
    }

    std::vector<TextEdit> edits;
    if (const jsx::JsxAttribute* existing = find_attribute(*element, "style");
        existing != nullptr) {
        // Replace exactly the attribute's bytes: its name, `=`, and its value. Everything around it
        // - the whitespace, the other attributes, the comments - is untouched.
        if (!is_code_point_boundary(text, existing->span.offset) ||
            !is_code_point_boundary(text, existing->span.offset + existing->span.length)) {
            return invalid_range("the `style` attribute's span is not on a code point boundary");
        }
        edits.push_back(TextEdit{context.file, existing->span.offset, existing->span.length,
                                 style_attribute_text(text, *element, request.declarations)});
        return edits;
    }

    // No `style` attribute: insert one at the end of the opening tag, before the `>` or `/>` that
    // closes it, so the element keeps its shape and its children are untouched.
    const std::uint32_t insert_at = opening_tag_end(text, element->span.offset);
    if (insert_at == 0) {
        return invalid_range("the element's opening tag has no end to insert at");
    }
    edits.push_back(TextEdit{context.file, insert_at, 0,
                             " " + style_attribute_text(text, *element, request.declarations)});
    return edits;
}

} // namespace tessera::compiler::codegen::cli::internal
