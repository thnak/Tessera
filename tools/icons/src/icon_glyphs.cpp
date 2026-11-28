// SPDX-License-Identifier: MIT

#include "icon_glyphs.hpp"

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::icons {
namespace {

struct GlyphPath {
    std::string d;
    std::string fill = "currentColor"; // SVG's default paint, resolved to the text color token
    std::string stroke = "none";
    std::string stroke_width = "1";
};

struct Glyph {
    std::string view_box;
    std::vector<GlyphPath> paths;
};

[[nodiscard]] bool plain_text(std::string_view value) {
    for (const char c : value) {
        if (c == '"' || c == '{' || c == '}' || c == '\\' || c == '<' || c == '>') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_number(std::string_view text) {
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

struct Attribute {
    std::string_view name;
    std::string_view value;
};

// Reads `name="value"` pairs; false on anything malformed (single quotes, unquoted values).
[[nodiscard]] bool read_attributes(std::string_view text, std::vector<Attribute>& out) {
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && (text[index] == ' ' || text[index] == '\n' ||
                                       text[index] == '\t' || text[index] == '/')) {
            ++index;
        }
        if (index >= text.size()) {
            break;
        }
        const std::size_t name_begin = index;
        while (index < text.size() && text[index] != '=' && text[index] != ' ') {
            ++index;
        }
        if (index + 1 >= text.size() || text[index] != '=' || text[index + 1] != '"') {
            return false;
        }
        const std::string_view name = text.substr(name_begin, index - name_begin);
        index += 2;
        const std::size_t value_begin = index;
        while (index < text.size() && text[index] != '"') {
            ++index;
        }
        if (index >= text.size()) {
            return false;
        }
        out.push_back({name, text.substr(value_begin, index - value_begin)});
        ++index;
    }
    return true;
}

[[nodiscard]] std::optional<Glyph> read_glyph(std::string_view svg) {
    Glyph glyph;
    std::size_t index = 0;
    bool saw_root = false;
    while (true) {
        const std::size_t open = svg.find('<', index);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t close = svg.find('>', open);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view tag = svg.substr(open + 1, close - open - 1);
        index = close + 1;
        if (tag.starts_with('/')) {
            continue; // a closing tag; only `svg` closes (a `path` is self-closing)
        }
        const std::size_t name_end = tag.find_first_of(" \n\t/");
        const std::string_view name = tag.substr(0, name_end);
        const std::string_view rest =
            name_end == std::string_view::npos ? std::string_view() : tag.substr(name_end);
        std::vector<Attribute> attributes;
        if (name != "svg" && name != "path") {
            return std::nullopt;
        }
        if (!read_attributes(rest, attributes)) {
            return std::nullopt;
        }
        if (name == "svg") {
            saw_root = true;
            for (const Attribute& attribute : attributes) {
                if (attribute.name == "viewBox" && plain_text(attribute.value)) {
                    glyph.view_box = std::string(attribute.value);
                }
            }
            continue;
        }
        GlyphPath path;
        for (const Attribute& attribute : attributes) {
            if (!plain_text(attribute.value) || attribute.value.starts_with("var(") ||
                attribute.value.starts_with("url(")) {
                return std::nullopt;
            }
            if (attribute.name == "d") {
                path.d = std::string(attribute.value);
            } else if (attribute.name == "fill") {
                path.fill = std::string(attribute.value);
            } else if (attribute.name == "stroke") {
                path.stroke = std::string(attribute.value);
            } else if (attribute.name == "stroke-width") {
                if (!is_number(attribute.value)) {
                    return std::nullopt;
                }
                path.stroke_width = std::string(attribute.value);
            } else if (attribute.name == "stroke-linecap" || attribute.name == "stroke-linejoin" ||
                       attribute.name == "fill-rule" || attribute.name == "clip-rule") {
                // The `Path` intrinsic has no cap, join or rule props: these draw with the
                // defaults. A known fidelity gap, documented in docs/specs/components/Icon.md.
            } else {
                return std::nullopt;
            }
        }
        if (path.d.empty()) {
            return std::nullopt;
        }
        glyph.paths.push_back(std::move(path));
    }
    if (!saw_root || glyph.view_box.empty() || glyph.paths.empty()) {
        return std::nullopt;
    }
    return glyph;
}

} // namespace

std::string build_icon_glyphs_tsx(const ImportResult& result) {
    std::string body;
    std::string skipped;
    for (const ImportedIcon& icon : result.icons) {
        const EmittedFile* document = nullptr;
        for (const EmittedFile& file : result.files) {
            if (file.path == icon.asset) {
                document = &file;
                break;
            }
        }
        const std::string name = icon.prefix + ":" + icon.name;
        const std::optional<Glyph> glyph =
            document == nullptr ? std::nullopt : read_glyph(document->bytes);
        if (!glyph.has_value()) {
            skipped.append("//   " + name + "\n");
            continue;
        }
        for (const GlyphPath& path : glyph->paths) {
            body.append("      {name === \"" + name + "\" && (\n");
            body.append("        <Path\n");
            body.append("          className=\"ticon-path\"\n");
            body.append("          d=\"" + path.d + "\"\n");
            body.append("          viewBox=\"" + glyph->view_box + "\"\n");
            body.append("          fill=\"" + path.fill + "\"\n");
            body.append("          stroke=\"" + path.stroke + "\"\n");
            body.append("          strokeWidth={" + path.stroke_width + "}\n");
            body.append("        />\n");
            body.append("      )}\n");
        }
    }
    std::string text;
    text.append("// SPDX-License-Identifier: MIT\n");
    text.append("//\n");
    text.append("// Generated by `tessera icons add`; do not edit.\n");
    text.append("//\n");
    text.append("// The imported icons as `Path` nodes, selected by `<set>:<name>`. The `Icon` "
                "component\n");
    text.append("// wraps this; see docs/specs/components/Icon.md.\n");
    if (!skipped.empty()) {
        text.append("//\n");
        text.append("// Not representable as `Path` nodes (shapes, groups or unknown attributes), "
                    "so absent\n");
        text.append("// here; their embedded SVG assets are still emitted:\n");
        text.append(skipped);
    }
    text.append("\nexport type IconGlyphProps = {\n  name: string;\n};\n\n");
    text.append("export function IconGlyph({ name }: IconGlyphProps) {\n");
    text.append("  return (\n");
    // The paths are drawing, not content: the Icon's box carries the name, so the glyph is hidden
    // (otherwise every path of a named icon would be an unnamed image in the tree).
    text.append("    <View className=\"ticon-glyph\" aria-hidden={true}>\n");
    text.append(body);
    text.append("    </View>\n");
    text.append("  );\n");
    text.append("}\n");
    return text;
}

} // namespace tessera::icons
