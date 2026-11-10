// SPDX-License-Identifier: MIT

#include "tessera/ui/automation/snapshot_json.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::ui::automation {
namespace {

constexpr double kBoundsScale = 1000.0;
constexpr double kBoundsLimit = 1.0e9;

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char character : text) {
        switch (character) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default: {
            const unsigned char code = static_cast<unsigned char>(character);
            if (code < 0x20U) {
                constexpr std::string_view kHexDigits = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHexDigits[code >> 4U]);
                out.push_back(kHexDigits[code & 0x0FU]);
            } else {
                out.push_back(character);
            }
            break;
        }
        }
    }
    out.push_back('"');
}

void append_decimal_id(std::string& out, std::uint64_t value) {
    append_quoted(out, std::to_string(value));
}

std::string fixed_3(float value) {
    if (!std::isfinite(value)) {
        return "0.000";
    }
    const bool negative = value < 0.0f;
    double magnitude = std::fabs(static_cast<double>(value));
    if (magnitude > kBoundsLimit) {
        magnitude = kBoundsLimit;
    }
    const auto thousandths = static_cast<long long>(magnitude * kBoundsScale + 0.5);
    std::string text;
    if (negative && thousandths != 0) {
        text.push_back('-');
    }
    text += std::to_string(thousandths / 1000);
    text.push_back('.');
    const long long fraction = thousandths % 1000;
    text.push_back(static_cast<char>('0' + (fraction / 100)));
    text.push_back(static_cast<char>('0' + ((fraction / 10) % 10)));
    text.push_back(static_cast<char>('0' + (fraction % 10)));
    return text;
}

void append_bounds(std::string& out, const SnapshotBounds& bounds) {
    out += "{\"x\":";
    out += fixed_3(bounds.x);
    out += ",\"y\":";
    out += fixed_3(bounds.y);
    out += ",\"width\":";
    out += fixed_3(bounds.width);
    out += ",\"height\":";
    out += fixed_3(bounds.height);
    out.push_back('}');
}

void append_node(std::string& out, const UiSnapshotNode& node) {
    out += "{\"uiId\":";
    append_decimal_id(out, node.ui_id);
    out += ",\"componentId\":";
    append_decimal_id(out, node.component_id);
    out += ",\"parent\":";
    if (node.parent == kNoIndex) {
        out += "null";
    } else {
        out += std::to_string(node.parent);
    }
    out += ",\"depth\":";
    out += std::to_string(node.depth);
    out += ",\"itemKey\":";
    append_quoted(out, node.item_key);
    out += ",\"kind\":";
    append_quoted(out, node.kind);
    out += ",\"path\":";
    append_quoted(out, node.path);
    out += ",\"name\":";
    append_quoted(out, node.name);
    out += ",\"text\":";
    append_quoted(out, node.text);
    out += ",\"bounds\":";
    append_bounds(out, node.bounds);
    out += ",\"visible\":";
    out += node.visible ? "true" : "false";
    out += ",\"enabled\":";
    out += node.enabled ? "true" : "false";
    out += ",\"focused\":";
    out += node.focused ? "true" : "false";
    out += ",\"selected\":";
    out += node.selected ? "true" : "false";
    out.push_back('}');
}

} // namespace

std::string snapshot_to_json(const UiSnapshot& snapshot) {
    std::string json;
    json.reserve(128 + snapshot.nodes.size() * 320);
    json += "{\"snapshotVersion\":";
    json += std::to_string(snapshot.version);
    json += ",\"sequence\":";
    append_decimal_id(json, snapshot.sequence);
    json += ",\"nodes\":[";
    for (std::size_t index = 0; index < snapshot.nodes.size(); ++index) {
        if (index != 0) {
            json.push_back(',');
        }
        append_node(json, snapshot.nodes[index]);
    }
    json += "]}\n";
    return json;
}

} // namespace tessera::ui::automation
