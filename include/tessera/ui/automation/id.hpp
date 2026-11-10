// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::automation {

// Numeric automation identity (docs/specs/automation-ids-v1.md sections 1-3). `UiId` and
// `ComponentId` are build-time stable: the same salt, source path and node/component name always
// produce the same 64-bit value, on every platform and build host.
using UiId = std::uint64_t;
using ComponentId = std::uint64_t;

// hash64 = the first eight SHA-256 digest bytes read as a big-endian uint64 (section 2). Input is
// the salted UTF-8 key built by `make_ui_id_key`/`make_component_id_key`, never a bare name.
[[nodiscard]] std::uint64_t hash64(std::span<const std::uint8_t> salted_key) noexcept;
[[nodiscard]] std::uint64_t hash64(std::string_view salted_key) noexcept;

// Salted key composition, shared so the compiler, codegen and manifest cannot drift:
//   ui key        = salt + "|u|" + source_path + "|" + node_path + "|" + key
//   component key = salt + "|c|" + source_path + "|" + component_name
// The trailing "|" keeps an empty key distinct from a key that ends at the node path.
[[nodiscard]] std::string make_ui_id_key(std::string_view salt, std::string_view source_path,
                                         std::string_view node_path, std::string_view key);
[[nodiscard]] std::string make_component_id_key(std::string_view salt, std::string_view source_path,
                                                std::string_view component_name);

enum class IdErrorCode : std::uint8_t {
    EmptySalt = 0,         // no tessera.json namespace: IDs would not be stable across projects
    EmptySourcePath,       // a node/component without a source file
    EmptyNodePath,         // a node without a structural path
    EmptyComponentName,    // a component without a name
    ZeroAutomationId,      // automationId="0" is reserved for "unset"
    DuplicateAutomationId, // explicit override reused by another node
    IdCollision,           // two distinct keys produced the same 64-bit id
};

[[nodiscard]] std::string_view to_string(IdErrorCode code) noexcept;

struct IdError {
    IdErrorCode code = IdErrorCode::EmptySalt;
    std::string detail; // human-readable, deterministic, names the offending inputs
};

// One node's identity inputs, in source/document order. `key` is the list/template key where
// applicable; `automation_id` is the optional explicit override.
struct NodeIdInput {
    std::string source_path;
    std::string node_path;
    std::string key;
    std::optional<std::uint64_t> automation_id;
};

struct AssignedNodeId {
    UiId ui_id = 0;
    bool override = false; // true when an explicit automationId supplied the value
};

// Assigns one UiId per input, in order. An explicit `automation_id` replaces the computed id
// (section 2/3). Duplicate overrides and 64-bit collisions are hard errors: ids are never
// renumbered or silently last-wins.
[[nodiscard]] std::expected<std::vector<AssignedNodeId>, IdError>
assign_ui_ids(std::string_view salt, std::span<const NodeIdInput> nodes);

struct ComponentIdInput {
    std::string source_path;
    std::string name;
};

// Component ids use the same salted-hash rule; a collision is a hard error as well.
[[nodiscard]] std::expected<std::vector<ComponentId>, IdError>
assign_component_ids(std::string_view salt, std::span<const ComponentIdInput> components);

} // namespace tessera::ui::automation
