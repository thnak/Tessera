// SPDX-License-Identifier: MIT

#include "tessera/ui/automation/id.hpp"
#include "tessera/ui/automation/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tessera::ui::automation {
namespace {

std::string describe_node(std::size_t index, const NodeIdInput& node) {
    std::string text = "node ";
    text += std::to_string(index);
    text += " (`";
    text += node.source_path;
    text += "` / `";
    text += node.node_path;
    text += "`)";
    return text;
}

std::string describe_component(std::size_t index, const ComponentIdInput& component) {
    std::string text = "component ";
    text += std::to_string(index);
    text += " (`";
    text += component.source_path;
    text += "` / `";
    text += component.name;
    text += "`)";
    return text;
}

std::expected<void, IdError> require_salt(std::string_view salt) {
    if (!salt.empty()) {
        return {};
    }
    return std::unexpected(IdError{
        IdErrorCode::EmptySalt,
        "the project salt (tessera.json namespace) is required for stable automation ids",
    });
}

} // namespace

std::uint64_t hash64(std::span<const std::uint8_t> salted_key) noexcept {
    const Sha256Digest digest = sha256(salted_key);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(digest.bytes[index]);
    }
    return value;
}

std::uint64_t hash64(std::string_view salted_key) noexcept {
    return hash64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(salted_key.data()), salted_key.size()));
}

std::string make_ui_id_key(std::string_view salt, std::string_view source_path,
                           std::string_view node_path, std::string_view key) {
    std::string salted;
    salted.reserve(salt.size() + source_path.size() + node_path.size() + key.size() + 5);
    salted += salt;
    salted += "|u|";
    salted += source_path;
    salted += '|';
    salted += node_path;
    salted += '|';
    salted += key;
    return salted;
}

std::string make_component_id_key(std::string_view salt, std::string_view source_path,
                                  std::string_view component_name) {
    std::string salted;
    salted.reserve(salt.size() + source_path.size() + component_name.size() + 4);
    salted += salt;
    salted += "|c|";
    salted += source_path;
    salted += '|';
    salted += component_name;
    return salted;
}

std::string_view to_string(IdErrorCode code) noexcept {
    switch (code) {
    case IdErrorCode::EmptySalt:
        return "empty_salt";
    case IdErrorCode::EmptySourcePath:
        return "empty_source_path";
    case IdErrorCode::EmptyNodePath:
        return "empty_node_path";
    case IdErrorCode::EmptyComponentName:
        return "empty_component_name";
    case IdErrorCode::ZeroAutomationId:
        return "zero_automation_id";
    case IdErrorCode::DuplicateAutomationId:
        return "duplicate_automation_id";
    case IdErrorCode::IdCollision:
        return "id_collision";
    }
    return "unknown";
}

std::expected<std::vector<AssignedNodeId>, IdError>
assign_ui_ids(std::string_view salt, std::span<const NodeIdInput> nodes) {
    if (const auto salt_check = require_salt(salt); !salt_check.has_value()) {
        return std::unexpected(salt_check.error());
    }

    std::vector<AssignedNodeId> assigned;
    assigned.reserve(nodes.size());
    std::unordered_map<std::uint64_t, std::size_t> first_use;
    first_use.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const NodeIdInput& node = nodes[index];
        if (node.source_path.empty()) {
            return std::unexpected(IdError{IdErrorCode::EmptySourcePath,
                                           describe_node(index, node) + " has no source path"});
        }
        if (node.node_path.empty()) {
            return std::unexpected(IdError{IdErrorCode::EmptyNodePath,
                                           describe_node(index, node) + " has no node path"});
        }

        AssignedNodeId entry;
        if (node.automation_id.has_value()) {
            if (*node.automation_id == 0) {
                return std::unexpected(
                    IdError{IdErrorCode::ZeroAutomationId,
                            describe_node(index, node) +
                                " uses automationId 0, which means \"unset\"; use a real id"});
            }
            entry.ui_id = *node.automation_id;
            entry.override = true;
        } else {
            entry.ui_id = hash64(make_ui_id_key(salt, node.source_path, node.node_path, node.key));
        }

        const auto [existing, inserted] = first_use.try_emplace(entry.ui_id, index);
        if (!inserted) {
            const std::size_t first = existing->second;
            const IdErrorCode code =
                entry.override ? IdErrorCode::DuplicateAutomationId : IdErrorCode::IdCollision;
            std::string detail = entry.override ? "duplicate automationId " : "id collision on ";
            detail += std::to_string(entry.ui_id);
            detail += ": ";
            detail += describe_node(index, node);
            detail += " collides with ";
            detail += describe_node(first, nodes[first]);
            detail += "; give the node a unique automationId or rename it";
            return std::unexpected(IdError{code, std::move(detail)});
        }
        assigned.push_back(entry);
    }
    return assigned;
}

std::expected<std::vector<ComponentId>, IdError>
assign_component_ids(std::string_view salt, std::span<const ComponentIdInput> components) {
    if (const auto salt_check = require_salt(salt); !salt_check.has_value()) {
        return std::unexpected(salt_check.error());
    }

    std::vector<ComponentId> assigned;
    assigned.reserve(components.size());
    std::unordered_map<ComponentId, std::size_t> first_use;
    first_use.reserve(components.size());
    for (std::size_t index = 0; index < components.size(); ++index) {
        const ComponentIdInput& component = components[index];
        if (component.source_path.empty()) {
            return std::unexpected(
                IdError{IdErrorCode::EmptySourcePath,
                        describe_component(index, component) + " has no source path"});
        }
        if (component.name.empty()) {
            return std::unexpected(
                IdError{IdErrorCode::EmptyComponentName,
                        describe_component(index, component) + " has no component name"});
        }

        const ComponentId id =
            hash64(make_component_id_key(salt, component.source_path, component.name));
        const auto [existing, inserted] = first_use.try_emplace(id, index);
        if (!inserted) {
            const std::size_t first = existing->second;
            std::string detail = "id collision on ";
            detail += std::to_string(id);
            detail += ": ";
            detail += describe_component(index, component);
            detail += " collides with ";
            detail += describe_component(first, components[first]);
            return std::unexpected(IdError{IdErrorCode::IdCollision, std::move(detail)});
        }
        assigned.push_back(id);
    }
    return assigned;
}

} // namespace tessera::ui::automation
