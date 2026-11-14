// SPDX-License-Identifier: MIT

#include "icon_set.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_value.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::icons {
namespace {

namespace harness = tessera::harness;

[[nodiscard]] std::optional<std::string> read_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::expected<harness::JsonValue, std::string>
read_json_file(const std::string& path) {
    const std::optional<std::string> text = read_text_file(path);
    if (!text) {
        return std::unexpected("cannot read " + path);
    }
    const auto parsed = harness::parse_json(*text);
    if (!parsed) {
        return std::unexpected("malformed JSON in " + path);
    }
    if (!parsed->is_object()) {
        return std::unexpected("expected a JSON object in " + path);
    }
    return *parsed;
}

[[nodiscard]] std::string string_of(const harness::JsonValue& object, std::string_view key) {
    const harness::JsonValue* const value = object.find(key);
    if (value == nullptr) {
        return {};
    }
    const std::optional<std::string_view> text = value->as_string();
    return text ? std::string(*text) : std::string();
}

[[nodiscard]] std::uint32_t number_of(const harness::JsonValue& object, std::string_view key) {
    const harness::JsonValue* const value = object.find(key);
    if (value == nullptr) {
        return 0;
    }
    const std::optional<std::int64_t> integer = value->as_integer();
    if (integer && *integer > 0) {
        return static_cast<std::uint32_t>(*integer);
    }
    const std::optional<double> real = value->as_number();
    if (real && *real > 0.0) {
        return static_cast<std::uint32_t>(*real);
    }
    return 0;
}

[[nodiscard]] bool bool_of(const harness::JsonValue& object, std::string_view key) {
    const harness::JsonValue* const value = object.find(key);
    if (value == nullptr) {
        return false;
    }
    const std::optional<bool> flag = value->as_bool();
    return flag.value_or(false);
}

} // namespace

const IconSource* IconSet::find(std::string_view name) const {
    const auto position = std::lower_bound(
        icons.begin(), icons.end(), name,
        [](const IconSource& icon, std::string_view wanted) { return icon.name < wanted; });
    if (position == icons.end() || position->name != name) {
        return nullptr;
    }
    return &*position;
}

std::expected<IconSet, std::string> load_icon_set(const std::string& root,
                                                  std::string_view prefix) {
    const std::string set_root = root + "/" + std::string(prefix);
    const auto info = read_json_file(set_root + "/info.json");
    if (!info) {
        return std::unexpected(info.error());
    }
    const auto icons_json = read_json_file(set_root + "/icons.json");
    if (!icons_json) {
        return std::unexpected(icons_json.error());
    }

    IconSet set;
    set.prefix = string_of(*info, "prefix");
    if (set.prefix.empty()) {
        set.prefix = std::string(prefix);
    }
    set.display_name = string_of(*info, "name");
    set.width = number_of(*info, "width");
    set.height = number_of(*info, "height");
    set.palette = bool_of(*info, "palette");
    if (const harness::JsonValue* const author = info->find("author"); author != nullptr) {
        set.author = string_of(*author, "name");
        set.author_url = string_of(*author, "url");
    }
    if (const harness::JsonValue* const license = info->find("license"); license != nullptr) {
        set.license_title = string_of(*license, "title");
        set.license_spdx = string_of(*license, "spdx");
        set.license_url = string_of(*license, "url");
    }
    // icons.json carries the drawing viewBox the per-icon entries default to.
    const std::uint32_t default_width = number_of(*icons_json, "width");
    const std::uint32_t default_height = number_of(*icons_json, "height");
    const std::uint32_t default_left = number_of(*icons_json, "left");
    const std::uint32_t default_top = number_of(*icons_json, "top");
    if (set.width == 0) {
        set.width = default_width;
    }
    if (set.height == 0) {
        set.height = default_height;
    }
    if (set.width == 0 || set.height == 0) {
        return std::unexpected("set " + std::string(prefix) +
                               " has no usable viewBox size in info.json or icons.json");
    }

    const harness::JsonValue* const icons = icons_json->find("icons");
    if (icons == nullptr || !icons->is_object()) {
        return std::unexpected("set " + std::string(prefix) + " has no icons object");
    }
    for (const harness::JsonValue::Member& member : *icons->as_object()) {
        if (bool_of(member.second, "hidden")) {
            continue;
        }
        IconSource icon;
        icon.name = member.first;
        icon.body = string_of(member.second, "body");
        if (icon.body.empty()) {
            return std::unexpected("icon " + std::string(prefix) + ":" + icon.name +
                                   " has no body");
        }
        icon.left = number_of(member.second, "left");
        icon.top = number_of(member.second, "top");
        icon.width = number_of(member.second, "width");
        icon.height = number_of(member.second, "height");
        if (icon.width == 0) {
            icon.width = default_width == 0 ? set.width : default_width;
            icon.left = default_left;
        }
        if (icon.height == 0) {
            icon.height = default_height == 0 ? set.height : default_height;
            icon.top = default_top;
        }
        set.icons.push_back(std::move(icon));
    }

    // Aliases resolve to the parent icon's body. Iconify allows rotate/hFlip/vFlip on an alias; the
    // converter refuses those rather than silently drawing a different icon.
    if (const harness::JsonValue* const aliases = icons_json->find("aliases");
        aliases != nullptr && aliases->is_object()) {
        for (const harness::JsonValue::Member& member : *aliases->as_object()) {
            if (bool_of(member.second, "hidden")) {
                continue;
            }
            const std::string parent = string_of(member.second, "parent");
            const IconSource* const source = set.find(parent);
            if (source == nullptr) {
                return std::unexpected("alias " + std::string(prefix) + ":" + member.first +
                                       " names an unknown parent \"" + parent + "\"");
            }
            const std::uint32_t rotate = number_of(member.second, "rotate");
            const bool h_flip = bool_of(member.second, "hFlip");
            const bool v_flip = bool_of(member.second, "vFlip");
            if (rotate != 0 || h_flip || v_flip) {
                return std::unexpected("alias " + std::string(prefix) + ":" + member.first +
                                       " carries rotate/hFlip/vFlip, which the importer does not "
                                       "apply; import the parent icon instead");
            }
            IconSource alias = *source;
            alias.name = member.first;
            set.icons.push_back(std::move(alias));
        }
    }
    std::sort(set.icons.begin(), set.icons.end(),
              [](const IconSource& lhs, const IconSource& rhs) { return lhs.name < rhs.name; });
    const auto duplicate = std::adjacent_find(
        set.icons.begin(), set.icons.end(),
        [](const IconSource& lhs, const IconSource& rhs) { return lhs.name == rhs.name; });
    if (duplicate != set.icons.end()) {
        return std::unexpected("set " + std::string(prefix) + " defines icon \"" + duplicate->name +
                               "\" twice");
    }
    return set;
}

std::string declared_license(const IconSet& set) {
    if (!set.license_spdx.empty()) {
        return set.license_spdx;
    }
    return set.license_title;
}

} // namespace tessera::icons
