// SPDX-License-Identifier: MIT

// The `token` destination of `source.patch` (issue #171, designer-protocol-v1 section 10.1): the
// value of the existing token declaration in the theme asset.
//
// **Why the token file is JSON and not CSS.** The token file is `assets.theme`, the theme asset of
// `stylesheet-v1.md` section 9.1: a JSON document whose `themes.<id>` objects map a token name to a
// string value. A token commit therefore edits a JSON string literal, not a CSS declaration, and
// this unit is the one place that difference is handled.
//
// **Why the value is located by scanning, not by re-serializing.** Re-serializing the asset would
// rewrite the whole file: key order, indentation, comments and every token the commit did not name
// would come back in the writer's shape. The spec's rule is that text outside the replaced range is
// never part of an edit (section 10.2), so the scan finds the exact byte range of the one string
// literal and replaces only that.
//
// **Why a missing token is a rejection.** Section 10.1 says the commit edits "the value of the
// existing token declaration", so a token the active theme does not define has nothing to edit. The
// closest of the spec's six reasons is `not_token_bound`: the node's override is not bound to a
// token this file can change.

#include "scene_canvas_patch_internal.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli::internal {
namespace {

// The byte range of the string literal that is the value of `token` inside the object of
// `theme_id`, or nullopt. `start` is the opening quote, `length` covers the quotes and the
// contents.
struct StringRange {
    std::uint32_t start = 0;
    std::uint32_t length = 0;
};

// Skips whitespace and `//` / `/* */` comments, which a hand-edited asset may carry even though
// strict JSON forbids them: the file is a project file a person edits, and refusing to locate a
// token because of a comment would make the destination unusable on the very files it targets.
std::size_t skip_trivia(std::string_view text, std::size_t index) {
    while (index < text.size()) {
        const char c = text[index];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++index;
            continue;
        }
        if (c == '/' && index + 1 < text.size() && text[index + 1] == '/') {
            index += 2;
            while (index < text.size() && text[index] != '\n') {
                ++index;
            }
            continue;
        }
        if (c == '/' && index + 1 < text.size() && text[index + 1] == '*') {
            index += 2;
            while (index + 1 < text.size() && !(text[index] == '*' && text[index + 1] == '/')) {
                ++index;
            }
            index = index + 1 < text.size() ? index + 2 : text.size();
            continue;
        }
        break;
    }
    return index;
}

// The end of the string literal starting at `index` (its opening quote), or 0 when it is
// unterminated. A backslash escapes the next byte, so `\"` does not close the string.
std::size_t string_end(std::string_view text, std::size_t index) {
    if (index >= text.size() || text[index] != '"') {
        return 0;
    }
    std::size_t cursor = index + 1;
    while (cursor < text.size()) {
        if (text[cursor] == '\\') {
            cursor += 2;
            continue;
        }
        if (text[cursor] == '"') {
            return cursor + 1;
        }
        ++cursor;
    }
    return 0;
}

// The decoded contents of the string literal at `[start, end)`, with the JSON escapes resolved.
// Only the escapes a token value can carry are handled; an unknown escape is kept verbatim, which
// is what a value the parser would reject should look like rather than a silently altered one.
std::string decode_string(std::string_view text, std::size_t start, std::size_t end) {
    std::string out;
    if (end <= start + 1) {
        return out;
    }
    for (std::size_t index = start + 1; index + 1 < end; ++index) {
        if (text[index] != '\\' || index + 2 >= end) {
            out.push_back(text[index]);
            continue;
        }
        ++index;
        switch (text[index]) {
        case 'n':
            out.push_back('\n');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case '"':
            out.push_back('"');
            break;
        case '\\':
            out.push_back('\\');
            break;
        case '/':
            out.push_back('/');
            break;
        default:
            out.push_back('\\');
            out.push_back(text[index]);
            break;
        }
    }
    return out;
}

// The byte offset where the value of the member named `key` begins, in the object starting at
// `object_start` (its `{`), or nullopt when the object has no such member. Only the object's own
// members are searched: a nested object's keys are skipped whole, so a token name that also appears
// inside a nested value cannot be mistaken for the declaration.
std::optional<std::size_t> member_value_start(std::string_view text, std::size_t object_start,
                                              std::string_view key) {
    if (object_start >= text.size() || text[object_start] != '{') {
        return std::nullopt;
    }
    std::size_t index = object_start + 1;
    while (true) {
        index = skip_trivia(text, index);
        if (index >= text.size() || text[index] == '}') {
            return std::nullopt;
        }
        if (text[index] == ',') {
            ++index;
            continue;
        }
        const std::size_t name_end = string_end(text, index);
        if (name_end == 0) {
            return std::nullopt;
        }
        const std::string name = decode_string(text, index, name_end);
        index = skip_trivia(text, name_end);
        if (index >= text.size() || text[index] != ':') {
            return std::nullopt;
        }
        index = skip_trivia(text, index + 1);
        if (name == key) {
            return index;
        }
        // Skip the value: a string, an object, an array or a scalar, balanced so a nested `}` does
        // not end the object early.
        if (index < text.size() && text[index] == '"') {
            index = string_end(text, index);
            if (index == 0) {
                return std::nullopt;
            }
            continue;
        }
        int depth = 0;
        while (index < text.size()) {
            const char c = text[index];
            if (c == '"') {
                index = string_end(text, index);
                if (index == 0) {
                    return std::nullopt;
                }
                continue;
            }
            if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (depth == 0) {
                    break;
                }
                --depth;
            } else if (c == ',' && depth == 0) {
                break;
            }
            ++index;
        }
    }
}

// The string literal that is the value of the member named `key`, or nullopt when the member is
// absent or is not a string. A token's value is always a string in the theme asset, so a non-string
// member is not a token declaration this destination can edit.
std::optional<StringRange> member_string(std::string_view text, std::size_t object_start,
                                         std::string_view key) {
    const std::optional<std::size_t> start = member_value_start(text, object_start, key);
    if (!start) {
        return std::nullopt;
    }
    const std::size_t end = string_end(text, *start);
    if (end == 0) {
        return std::nullopt;
    }
    return StringRange{static_cast<std::uint32_t>(*start),
                       static_cast<std::uint32_t>(end - *start)};
}

// The object of `theme_id` inside the asset's `themes` object, or nullopt. The active theme's own
// object is the only place a token commit may write: the same token in another theme is a different
// declaration, and editing it would change a theme the developer is not looking at.
std::optional<std::size_t> theme_object(std::string_view text, std::string_view theme_id) {
    const std::optional<std::size_t> themes = member_value_start(text, 0, "themes");
    if (!themes) {
        return std::nullopt;
    }
    const std::size_t themes_object = skip_trivia(text, *themes);
    if (themes_object >= text.size() || text[themes_object] != '{') {
        return std::nullopt;
    }
    const std::optional<std::size_t> theme = member_value_start(text, themes_object, theme_id);
    if (!theme) {
        return std::nullopt;
    }
    const std::size_t object = skip_trivia(text, *theme);
    if (object >= text.size() || text[object] != '{') {
        return std::nullopt;
    }
    return object;
}

// The JSON spelling of `value`, with the escapes a token value needs.
std::string encode_string(std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
    return out;
}

// True when `value` is `var(<token>)` with nothing else around it: the binding the spec requires
// before a token commit is allowed (section 10.1). Whitespace inside the parentheses is allowed,
// because `var( --accent )` is the same reference to the cascade.
bool is_var_of(std::string_view value, std::string_view token) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return false;
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    constexpr std::string_view kVar = "var(";
    if (value.size() < kVar.size() + 1 || value.substr(0, kVar.size()) != kVar ||
        value.back() != ')') {
        return false;
    }
    std::string_view inner = value.substr(kVar.size(), value.size() - kVar.size() - 1);
    const auto inner_first = inner.find_first_not_of(" \t\r\n");
    if (inner_first == std::string_view::npos) {
        return false;
    }
    const auto inner_last = inner.find_last_not_of(" \t\r\n");
    inner = inner.substr(inner_first, inner_last - inner_first + 1);
    return inner == token;
}

} // namespace

std::expected<std::vector<TextEdit>, PatchError> token_edits(const PatchRequest& request,
                                                             const PatchContext& context) {
    if (request.token.empty()) {
        return std::unexpected(
            PatchError{PatchErrorCode::NotTokenBound, "a `token` commit names no token"});
    }
    // The binding test: every declaration the commit writes must be `var(<token>)`, because the
    // spec's token destination edits the token a node's override is bound to. A declaration with a
    // literal value is not bound to any token, so there is nothing for this destination to change.
    for (const Declaration& declaration : request.declarations) {
        if (!is_var_of(declaration.second, request.token)) {
            return std::unexpected(PatchError{PatchErrorCode::NotTokenBound,
                                              "`" + declaration.first + ": " + declaration.second +
                                                  "` is not `var(" + request.token + ")`"});
        }
    }
    if (context.theme_file.empty() || context.theme_text.empty()) {
        return std::unexpected(
            PatchError{PatchErrorCode::NotTokenBound, "the project has no theme asset to edit"});
    }
    const std::optional<std::size_t> theme = theme_object(context.theme_text, context.theme_id);
    if (!theme) {
        return std::unexpected(
            PatchError{PatchErrorCode::NotTokenBound,
                       "the theme asset has no theme `" + context.theme_id + "`"});
    }
    const std::optional<StringRange> value =
        member_string(context.theme_text, *theme, request.token);
    if (!value) {
        // The spec edits the EXISTING declaration; a token the theme does not define has none, so
        // this is a rejection rather than an append (section 10.1).
        return std::unexpected(PatchError{PatchErrorCode::NotTokenBound,
                                          "the theme `" + context.theme_id + "` does not define `" +
                                              request.token + "`"});
    }
    if (!is_code_point_boundary(context.theme_text, value->start) ||
        !is_code_point_boundary(context.theme_text, value->start + value->length)) {
        return invalid_range("the token's value is not on a code point boundary");
    }
    if (request.token_value.empty()) {
        return std::unexpected(
            PatchError{PatchErrorCode::NotTokenBound,
                       "the commit carries no value for `" + request.token + "`"});
    }
    // The new value is the one the commit carries: the declarations name the binding, so the
    // token's own value is what the override resolved to, and that is what the developer is
    // committing.
    std::vector<TextEdit> edits;
    edits.push_back(TextEdit{context.theme_file, value->start, value->length,
                             encode_string(request.token_value)});
    return edits;
}

} // namespace tessera::compiler::codegen::cli::internal
