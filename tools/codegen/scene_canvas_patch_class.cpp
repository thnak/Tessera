// SPDX-License-Identifier: MIT

// The `class` destination of `source.patch` (issue #171, designer-protocol-v1 section 10.1): the
// rule for `.className` in the project CSS file the commit names.
//
// **Why the rule is located with the stylesheet parser.** `style::parse_stylesheet` is the same
// parser the preview compiles with, and it records each rule's byte offset and each declaration's
// byte offset. A rule the commit edits is therefore a rule the cascade would have read, at the
// offset the parser measured - not a `.className` found by string search, which would match a
// comment, a `@media` prelude or a longer class name that merely starts the same way.
//
// **Why an absent rule is appended rather than inserted.** The spec says the rule "is appended to
// that file if missing" (section 10.1). Appending is also the only insertion that cannot change the
// meaning of what is already there: a rule inserted in the middle of an `@media` block would move
// into it, and one inserted before a `@layer` statement would change the layer order.

#include "scene_canvas_patch_internal.hpp"
#include "tessera/ui/style/parser.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli::internal {
namespace {

namespace style = tessera::ui::style;

// The selector text a rule for `class_name` has: `.name`, with the class escaped the way a CSS
// identifier needs. A class name the subset accepts is identifier-shaped, so the escape is only for
// the characters a scoped CSS-module name can carry.
std::string class_selector(std::string_view class_name) {
    std::string out = ".";
    for (const char c : class_name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!plain) {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

// True when `selector` is exactly the rule for `class_name`: one class selector and nothing else.
// `.card:hover`, `.card .child` and `.card-2` are different rules, and a commit for `.card` must
// not rewrite any of them.
bool is_class_rule(std::string_view selector, std::string_view class_name) {
    return selector == class_selector(class_name);
}

// The declaration text for one property, as CSS: `property: value;`.
std::string declaration_text(std::string_view property, std::string_view value) {
    return std::string(property) + ": " + std::string(value) + ";";
}

// The whole rule text for a class that has none: `.name {\n  property: value;\n}\n`.
std::string new_rule_text(std::string_view class_name,
                          const std::vector<Declaration>& declarations) {
    std::string text = class_selector(class_name) + " {\n";
    for (const Declaration& declaration : declarations) {
        text += "  " + declaration_text(declaration.first, declaration.second) + "\n";
    }
    text += "}\n";
    return text;
}

} // namespace

std::expected<std::vector<TextEdit>, PatchError> class_edits(const PatchRequest& request,
                                                             const PatchContext& context) {
    if (request.class_name.empty()) {
        return std::unexpected(
            PatchError{PatchErrorCode::NoSuchClassFile, "a `class` commit names no class"});
    }
    if (request.file.empty()) {
        return std::unexpected(
            PatchError{PatchErrorCode::NoSuchClassFile, "a `class` commit names no CSS file"});
    }
    const std::string_view text = project_text(context.project(), request.file);
    if (text.empty()) {
        // The spec's rule: the file must already be a project file, because a commit never creates
        // a CSS file (section 10.1).
        return std::unexpected(PatchError{PatchErrorCode::NoSuchClassFile,
                                          "the project has no CSS file `" + request.file + "`"});
    }

    std::vector<TextEdit> edits;
    const style::StyleSheet sheet = style::parse_stylesheet(text);
    const style::Rule* rule = nullptr;
    for (const style::Rule& candidate : sheet.rules) {
        if (is_class_rule(candidate.selector, request.class_name)) {
            rule = &candidate;
            break;
        }
    }

    if (rule == nullptr) {
        // Append the whole rule. The edit is an insertion at the end of the file, so every byte the
        // developer wrote stays where it is; a file that does not end in a newline gets one first
        // so the new rule cannot join the last line.
        std::string appended;
        if (!text.empty() && text.back() != '\n') {
            appended.push_back('\n');
        }
        appended += new_rule_text(request.class_name, request.declarations);
        edits.push_back(
            TextEdit{request.file, static_cast<std::uint32_t>(text.size()), 0, appended});
        return edits;
    }

    // The rule exists: replace the declarations the commit names and insert the ones it adds. Each
    // declaration is its own edit, so a declaration the commit does not name keeps its exact bytes
    // (including its formatting and any comment beside it).
    for (const Declaration& declaration : request.declarations) {
        const style::Declaration* existing = nullptr;
        for (const style::Declaration& candidate : rule->declarations) {
            if (candidate.property == declaration.first) {
                existing = &candidate;
                break;
            }
        }
        if (existing != nullptr) {
            // The declaration's offset is its property name's first byte; the value follows the
            // colon. Replacing from the property name through the value's end keeps the
            // `!important` flag and the semicolon, which are not part of what the commit changes.
            const std::uint32_t start = static_cast<std::uint32_t>(existing->offset);
            const std::uint32_t end = declaration_end(text, start);
            if (end <= start) {
                return invalid_range("a declaration of `" + request.file + "` has no end");
            }
            if (!is_code_point_boundary(text, start) || !is_code_point_boundary(text, end)) {
                return invalid_range("a declaration's span is not on a code point boundary");
            }
            edits.push_back(TextEdit{request.file, start, end - start,
                                     declaration_text(declaration.first, declaration.second)});
            continue;
        }
        // A property the rule does not set: insert it just before the rule's closing brace, so the
        // rule keeps its own formatting and the new declaration lands inside it.
        const std::uint32_t insert_at =
            rule_block_end(text, static_cast<std::uint32_t>(rule->offset));
        if (insert_at == 0) {
            return invalid_range("the rule for `." + request.class_name + "` has no closing brace");
        }
        edits.push_back(
            TextEdit{request.file, insert_at, 0,
                     "  " + declaration_text(declaration.first, declaration.second) + "\n"});
    }
    return edits;
}

} // namespace tessera::compiler::codegen::cli::internal
