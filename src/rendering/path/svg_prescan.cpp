// SPDX-License-Identifier: MIT
//
// SVG pre-scan for the static-icon subset (see svg_prescan.hpp). Pure byte inspection; the ThorVG
// adapter calls it before handing the bytes to the loader.

#include "svg_prescan.hpp"

#include <cstddef>
#include <string_view>

namespace tessera::rendering::path {
namespace {

[[nodiscard]] bool ascii_iequals(std::string_view text, std::string_view lower) noexcept {
    if (text.size() != lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != lower[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ascii_contains_iequals(std::string_view text, std::string_view lower) noexcept {
    if (lower.empty() || text.size() < lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i + lower.size() <= text.size(); ++i) {
        if (ascii_iequals(text.substr(i, lower.size()), lower)) {
            return true;
        }
    }
    return false;
}

// Elements whose meaning we cannot represent. The loader would silently drop some of them (text,
// images) or decode a paint we cannot express (gradients); rejecting keeps a partial icon loud.
// `<title>`, `<desc>`, `<metadata>`, `<defs>`, `<style>` and unknown presentation attributes are
// ignored instead.
[[nodiscard]] bool is_unsupported_tag(std::string_view name) noexcept {
    static constexpr std::string_view kUnsupported[] = {
        "text",
        "tspan",
        "textpath",
        "image",
        "foreignobject",
        "filter",
        "lineargradient",
        "radialgradient",
        "conicgradient",
        "pattern",
        "mask",
        "clippath",
        "animate",
        "animatetransform",
        "animatemotion",
        "set",
        "use",
        "script",
        "switch",
        "marker",
    };
    if (name.size() >= 2 && (name[0] == 'f') && (name[1] == 'e')) {
        return true; // filter primitive (feGaussianBlur, ...)
    }
    for (const std::string_view unsupported : kUnsupported) {
        if (ascii_iequals(name, unsupported)) {
            return true;
        }
    }
    return false;
}

} // namespace

// Conservative pre-scan of tag names and references. It runs before ThorVG sees the bytes so a
// rejected asset is rejected for a documented, testable reason rather than by loader behavior.
std::expected<void, SvgError> scan_unsupported_svg(std::span<const std::uint8_t> bytes) {
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    const std::size_t size = bytes.size();
    std::size_t i = 0;
    while (i < size) {
        if (data[i] != '<') {
            ++i;
            continue;
        }
        // XML prolog, comments, CDATA and declarations.
        if (i + 3 < size && data[i + 1] == '!' && data[i + 2] == '-' && data[i + 3] == '-') {
            const std::string_view rest(data + i, size - i);
            const std::size_t end = rest.find("-->");
            if (end == std::string_view::npos) {
                return std::unexpected(SvgError::malformed);
            }
            i += end + 3;
            continue;
        }
        if (i + 8 < size && std::string_view(data + i, 9) == "<![CDATA[") {
            const std::string_view rest(data + i, size - i);
            const std::size_t end = rest.find("]]>");
            if (end == std::string_view::npos) {
                return std::unexpected(SvgError::malformed);
            }
            i += end + 3;
            continue;
        }
        if (i + 1 < size && data[i + 1] == '!') {
            // DOCTYPE and other declarations can pull in external entities/DTDs.
            return std::unexpected(SvgError::unsupported);
        }
        if (i + 1 < size && data[i + 1] == '?') {
            const std::string_view rest(data + i, size - i);
            const std::size_t end = rest.find("?>");
            if (end == std::string_view::npos) {
                return std::unexpected(SvgError::malformed);
            }
            i += end + 2;
            continue;
        }
        if (i + 1 < size && data[i + 1] == '/') {
            const std::string_view rest(data + i, size - i);
            const std::size_t end = rest.find('>');
            if (end == std::string_view::npos) {
                return std::unexpected(SvgError::malformed);
            }
            i += end + 1;
            continue;
        }

        // Opening tag: read the name, then the tag body (quote-aware) for references.
        std::size_t name_end = i + 1;
        while (name_end < size) {
            const char c = data[name_end];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/') {
                break;
            }
            ++name_end;
        }
        if (name_end == i + 1) {
            ++i; // a stray '<' that is not a tag start; let the loader decide
            continue;
        }
        const std::string_view name(data + i + 1, name_end - i - 1);
        if (is_unsupported_tag(name)) {
            return std::unexpected(SvgError::unsupported);
        }

        std::size_t end = name_end;
        char quote = 0;
        while (end < size) {
            const char c = data[end];
            if (quote != 0) {
                if (c == quote) {
                    quote = 0;
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            } else if (c == '<') {
                return std::unexpected(SvgError::malformed);
            }
            ++end;
        }
        if (end >= size) {
            return std::unexpected(SvgError::malformed);
        }
        const std::string_view tag_body(data + name_end, end - name_end);
        if (ascii_contains_iequals(tag_body, "href") || ascii_contains_iequals(tag_body, "url(")) {
            // External/internal IRI references (use, image, paint servers, clip paths) are out of
            // the static-icon subset; the tag-level rejects above catch most of them, this catches
            // presentation attributes and style declarations that reference a resource.
            return std::unexpected(SvgError::unsupported);
        }
        i = end + 1;
    }
    return {};
}

} // namespace tessera::rendering::path
