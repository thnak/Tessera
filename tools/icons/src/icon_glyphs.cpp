// SPDX-License-Identifier: MIT

#include "icon_glyphs.hpp"

#include <algorithm>
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
    // Empty means unset: the prop is left out and the SVG default (butt, miter, nonzero) applies.
    std::string linecap;
    std::string linejoin;
    std::string fill_rule;
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

[[nodiscard]] std::optional<double> number_of(std::string_view text) {
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string number_text(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, result.ptr);
}

// The geometry attributes of a basic shape, by name; a missing one reads as `fallback`.
struct Geometry {
    const std::vector<Attribute>& attributes;

    [[nodiscard]] const Attribute* find(std::string_view name) const {
        for (const Attribute& attribute : attributes) {
            if (attribute.name == name) {
                return &attribute;
            }
        }
        return nullptr;
    }
    // nullopt when the attribute is present but not a plain number (a unit, a percentage).
    [[nodiscard]] std::optional<double> get(std::string_view name, double fallback) const {
        const Attribute* attribute = find(name);
        return attribute == nullptr ? std::optional<double>(fallback) : number_of(attribute->value);
    }
};

[[nodiscard]] std::string join_path(std::initializer_list<std::string> parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.append(part);
    }
    return out;
}

[[nodiscard]] std::string n(double value) {
    return number_text(value);
}

// Circle and ellipse are two half-turn arcs, the form SVG's own shape-to-path rules produce.
[[nodiscard]] std::optional<std::string> ellipse_path(double cx, double cy, double rx, double ry) {
    if (!(rx > 0.0) || !(ry > 0.0)) {
        return std::nullopt;
    }
    const std::string radii = n(rx) + " " + n(ry) + " 0 1 0";
    return join_path({"M" + n(cx - rx) + " " + n(cy), "a" + radii, n(2 * rx) + " 0", "a" + radii,
                      n(-2 * rx) + " 0", "z"});
}

[[nodiscard]] std::optional<std::string> rect_path(const Geometry& g) {
    const auto x = g.get("x", 0.0);
    const auto y = g.get("y", 0.0);
    const auto w = g.get("width", 0.0);
    const auto h = g.get("height", 0.0);
    if (!x || !y || !w || !h || !(*w > 0.0) || !(*h > 0.0)) {
        return std::nullopt;
    }
    std::optional<double> rx = g.find("rx") != nullptr ? g.get("rx", 0.0) : std::nullopt;
    std::optional<double> ry = g.find("ry") != nullptr ? g.get("ry", 0.0) : std::nullopt;
    if ((g.find("rx") != nullptr && !rx) || (g.find("ry") != nullptr && !ry)) {
        return std::nullopt;
    }
    // A single radius applies to both axes; both are clamped to half the side.
    const double radius_x = std::min(rx.value_or(ry.value_or(0.0)), *w / 2);
    const double radius_y = std::min(ry.value_or(rx.value_or(0.0)), *h / 2);
    if (radius_x <= 0.0 || radius_y <= 0.0) {
        return join_path({"M" + n(*x) + " " + n(*y), "h" + n(*w), "v" + n(*h), "h" + n(-*w), "z"});
    }
    const std::string arc = "a" + n(radius_x) + " " + n(radius_y) + " 0 0 1 ";
    return join_path({"M" + n(*x + radius_x) + " " + n(*y), "h" + n(*w - 2 * radius_x),
                      arc + n(radius_x) + " " + n(radius_y), "v" + n(*h - 2 * radius_y),
                      arc + n(-radius_x) + " " + n(radius_y), "h" + n(-(*w - 2 * radius_x)),
                      arc + n(-radius_x) + " " + n(-radius_y), "v" + n(-(*h - 2 * radius_y)),
                      arc + n(radius_x) + " " + n(-radius_y), "z"});
}

// `points` is numbers separated by whitespace or commas; an odd count is malformed.
[[nodiscard]] std::optional<std::string> points_path(std::string_view points, bool close) {
    std::vector<double> values;
    std::size_t index = 0;
    while (index < points.size()) {
        while (index < points.size() && (points[index] == ' ' || points[index] == ',' ||
                                         points[index] == '\n' || points[index] == '\t')) {
            ++index;
        }
        const std::size_t begin = index;
        while (index < points.size() && points[index] != ' ' && points[index] != ',' &&
               points[index] != '\n' && points[index] != '\t') {
            ++index;
        }
        if (index == begin) {
            break;
        }
        const auto value = number_of(points.substr(begin, index - begin));
        if (!value) {
            return std::nullopt;
        }
        values.push_back(*value);
    }
    if (values.size() < 4 || values.size() % 2 != 0) {
        return std::nullopt;
    }
    std::string out;
    for (std::size_t i = 0; i < values.size(); i += 2) {
        out.append(i == 0 ? "M" : " L");
        out.append(n(values[i]) + " " + n(values[i + 1]));
    }
    if (close) {
        out.append(" z");
    }
    return out;
}

// The path data of a basic shape, or nullopt for a malformed shape or an unsupported tag.
[[nodiscard]] std::optional<std::string> shape_path(std::string_view tag,
                                                    const std::vector<Attribute>& attributes) {
    const Geometry g{attributes};
    if (tag == "circle") {
        const auto cx = g.get("cx", 0.0);
        const auto cy = g.get("cy", 0.0);
        const auto r = g.get("r", 0.0);
        return cx && cy && r ? ellipse_path(*cx, *cy, *r, *r) : std::nullopt;
    }
    if (tag == "ellipse") {
        const auto cx = g.get("cx", 0.0);
        const auto cy = g.get("cy", 0.0);
        const auto rx = g.get("rx", 0.0);
        const auto ry = g.get("ry", 0.0);
        return cx && cy && rx && ry ? ellipse_path(*cx, *cy, *rx, *ry) : std::nullopt;
    }
    if (tag == "rect") {
        return rect_path(g);
    }
    if (tag == "line") {
        const auto x1 = g.get("x1", 0.0);
        const auto y1 = g.get("y1", 0.0);
        const auto x2 = g.get("x2", 0.0);
        const auto y2 = g.get("y2", 0.0);
        if (!x1 || !y1 || !x2 || !y2) {
            return std::nullopt;
        }
        return "M" + n(*x1) + " " + n(*y1) + " L" + n(*x2) + " " + n(*y2);
    }
    if (tag == "polyline" || tag == "polygon") {
        const Attribute* points = g.find("points");
        return points == nullptr ? std::nullopt : points_path(points->value, tag == "polygon");
    }
    return std::nullopt;
}

[[nodiscard]] bool is_geometry_attribute(std::string_view name) {
    static constexpr std::string_view kNames[] = {
        "cx", "cy", "r", "rx", "ry", "x", "y", "width", "height", "x1", "y1", "x2", "y2", "points"};
    return std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames);
}

// Applies one presentation attribute to `path`. false: not a supported attribute or value.
[[nodiscard]] bool apply_style(GlyphPath& path, const Attribute& attribute) {
    if (!plain_text(attribute.value) || attribute.value.starts_with("var(") ||
        attribute.value.starts_with("url(")) {
        return false;
    }
    const std::string value(attribute.value);
    if (attribute.name == "fill") {
        path.fill = value;
    } else if (attribute.name == "stroke") {
        path.stroke = value;
    } else if (attribute.name == "stroke-width") {
        if (!is_number(attribute.value)) {
            return false;
        }
        path.stroke_width = value;
    } else if (attribute.name == "stroke-linecap") {
        if (value != "butt" && value != "round" && value != "square") {
            return false;
        }
        path.linecap = value;
    } else if (attribute.name == "stroke-linejoin") {
        if (value != "miter" && value != "round" && value != "bevel") {
            return false;
        }
        path.linejoin = value;
    } else if (attribute.name == "fill-rule") {
        if (value != "nonzero" && value != "evenodd") {
            return false;
        }
        path.fill_rule = value;
    } else if (attribute.name == "clip-rule" || attribute.name == "xmlns" ||
               attribute.name == "width" || attribute.name == "height") {
        // No clip paths exist here; the size comes from the box, not the document.
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<Glyph> read_glyph(std::string_view svg) {
    Glyph glyph;
    std::size_t index = 0;
    bool saw_root = false;
    GlyphPath root_style;
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
        const bool is_shape = name == "path" || name == "circle" || name == "ellipse" ||
                              name == "rect" || name == "line" || name == "polyline" ||
                              name == "polygon";
        if (name != "svg" && !is_shape) {
            return std::nullopt;
        }
        if (!read_attributes(rest, attributes)) {
            return std::nullopt;
        }
        if (name == "svg") {
            saw_root = true;
            for (const Attribute& attribute : attributes) {
                if (attribute.name == "viewBox") {
                    if (!plain_text(attribute.value)) {
                        return std::nullopt;
                    }
                    glyph.view_box = std::string(attribute.value);
                } else if (!apply_style(root_style, attribute)) {
                    return std::nullopt; // a root attribute the table cannot carry
                }
            }
            continue;
        }
        // Presentation attributes on the root are inherited by every shape.
        GlyphPath path = root_style;
        path.d.clear();
        for (const Attribute& attribute : attributes) {
            if (attribute.name == "d" && name == "path") {
                if (!plain_text(attribute.value)) {
                    return std::nullopt;
                }
                path.d = std::string(attribute.value);
            } else if (name != "path" && is_geometry_attribute(attribute.name)) {
                // Read by shape_path below.
            } else if (!apply_style(path, attribute)) {
                return std::nullopt;
            }
        }
        if (name != "path") {
            const std::optional<std::string> data = shape_path(name, attributes);
            if (!data) {
                return std::nullopt;
            }
            path.d = *data;
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
            if (!path.linecap.empty()) {
                body.append("          strokeLinecap=\"" + path.linecap + "\"\n");
            }
            if (!path.linejoin.empty()) {
                body.append("          strokeLinejoin=\"" + path.linejoin + "\"\n");
            }
            if (!path.fill_rule.empty()) {
                body.append("          fillRule=\"" + path.fill_rule + "\"\n");
            }
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
        text.append("// Not representable as `Path` nodes (groups, unknown tags or attributes), "
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
