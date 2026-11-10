// SPDX-License-Identifier: MIT

#include "tessera/ui/automation/id.hpp"
#include "tessera/ui/automation/sha256.hpp"

#include <cstdint>
#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::ui::automation::assign_component_ids;
using tessera::ui::automation::assign_ui_ids;
using tessera::ui::automation::AssignedNodeId;
using tessera::ui::automation::ComponentId;
using tessera::ui::automation::ComponentIdInput;
using tessera::ui::automation::hash64;
using tessera::ui::automation::IdError;
using tessera::ui::automation::IdErrorCode;
using tessera::ui::automation::make_component_id_key;
using tessera::ui::automation::make_ui_id_key;
using tessera::ui::automation::NodeIdInput;
using tessera::ui::automation::to_string;
using tessera::ui::automation::UiId;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "automation_hash_id: " << message << "\n";
    }
    return condition;
}

bool is_error(const std::expected<std::vector<AssignedNodeId>, IdError>& result, IdErrorCode code,
              std::string_view detail_fragment = {}) {
    if (result.has_value()) {
        return false;
    }
    if (result.error().code != code) {
        return false;
    }
    return detail_fragment.empty() ||
           result.error().detail.find(detail_fragment) != std::string::npos;
}

bool is_error(const std::expected<std::vector<ComponentId>, IdError>& result, IdErrorCode code,
              std::string_view detail_fragment = {}) {
    if (result.has_value()) {
        return false;
    }
    if (result.error().code != code) {
        return false;
    }
    return detail_fragment.empty() ||
           result.error().detail.find(detail_fragment) != std::string::npos;
}

NodeIdInput make_input(std::string source_path, std::string node_path,
                       std::string key = std::string(),
                       std::optional<std::uint64_t> automation_id = std::nullopt) {
    NodeIdInput input;
    input.source_path = std::move(source_path);
    input.node_path = std::move(node_path);
    input.key = std::move(key);
    input.automation_id = automation_id;
    return input;
}

NodeIdInput button_input(std::optional<std::uint64_t> automation_id = std::nullopt) {
    return make_input("ui/Counter.tsx", "Counter/root[0]/Button[1]", std::string(), automation_id);
}

} // namespace

int main() {
    // Salted key composition is exactly the formula in section 2 (the trailing '|' keeps an empty
    // key distinct from an unkeyed path segment).
    if (!expect(make_ui_id_key("factory", "ui/Counter.tsx", "Counter/root[0]/Button[1]", "") ==
                    "factory|u|ui/Counter.tsx|Counter/root[0]/Button[1]|",
                "ui key composition") ||
        !expect(make_ui_id_key("factory", "ui/Counter.tsx", "Counter/root[0]/Button[1]", "row-7") ==
                    "factory|u|ui/Counter.tsx|Counter/root[0]/Button[1]|row-7",
                "ui key composition with key") ||
        !expect(make_component_id_key("factory", "ui/Counter.tsx", "Counter") ==
                    "factory|c|ui/Counter.tsx|Counter",
                "component key composition")) {
        return 1;
    }

    // Known answers: SHA-256 over the salted keys, first eight digest bytes as a big-endian
    // uint64.
    if (!expect(hash64("") == 0xe3b0c44298fc1c14ULL, "hash64 of empty bytes") ||
        !expect(hash64("abc") == 0xba7816bf8f01cfeaULL, "hash64 of abc") ||
        !expect(hash64(make_ui_id_key("factory", "ui/Counter.tsx", "Counter/root[0]/Button[1]",
                                      "")) == 14737924309377709359ULL,
                "hash64 of the unkeyed Counter button") ||
        !expect(hash64(make_ui_id_key("factory", "ui/Counter.tsx", "Counter/root[0]/Button[1]",
                                      "row-7")) == 14029618068745451035ULL,
                "hash64 of the keyed Counter button") ||
        !expect(hash64(make_component_id_key("factory", "ui/Counter.tsx", "Counter")) ==
                    15449676450390438563ULL,
                "hash64 of the Counter component")) {
        return 2;
    }

    // The project salt is part of the key: a different tessera.json namespace yields a different
    // id for the same source path and node path.
    const UiId factory_id =
        hash64(make_ui_id_key("factory", "ui/Counter.tsx", "Counter/root[0]/Button[1]", ""));
    const UiId warehouse_id =
        hash64(make_ui_id_key("warehouse", "ui/Counter.tsx", "Counter/root[0]/Button[1]", ""));
    if (!expect(warehouse_id == 7603602193865160278ULL, "hash64 under another namespace") ||
        !expect(factory_id != warehouse_id, "namespace changes the id") ||
        !expect(hash64(make_ui_id_key("factory", "ui/Counter.tsx", "Counter/root[0]/Button[2]",
                                      "")) != factory_id,
                "node path changes the id") ||
        !expect(hash64(make_ui_id_key("factory", "ui/Other.tsx", "Counter/root[0]/Button[1]",
                                      "")) != factory_id,
                "source path changes the id") ||
        !expect(hash64(make_ui_id_key("factory", "ui/Counter.tsx", "Counter/root[0]/Button[1]",
                                      "row-7")) != factory_id,
                "key changes the id")) {
        return 3;
    }

    // Default assignment computes one id per node in order; no overrides.
    const std::vector<NodeIdInput> defaults{
        button_input(), make_input("ui/Counter.tsx", "Counter/root[1]"),
        make_input("ui/Counter.tsx", "Counter/root[2]", "row-7")};
    const auto default_result = assign_ui_ids("factory", defaults);
    if (!expect(default_result.has_value(), "default assignment succeeds") ||
        !expect(default_result->size() == 3, "one id per node") ||
        !expect((*default_result)[0].ui_id == factory_id, "computed button id") ||
        !expect((*default_result)[0].override == false, "computed id is not an override") ||
        !expect((*default_result)[2].ui_id ==
                    hash64(make_ui_id_key("factory", "ui/Counter.tsx", "Counter/root[2]", "row-7")),
                "computed keyed id")) {
        return 4;
    }

    // An explicit automationId replaces the computed id and is flagged as an override.
    const std::vector<NodeIdInput> overridden{button_input(42)};
    const auto override_result = assign_ui_ids("factory", overridden);
    if (!expect(override_result.has_value(), "override assignment succeeds") ||
        !expect((*override_result)[0].ui_id == 42, "explicit automationId wins") ||
        !expect((*override_result)[0].override, "override is flagged")) {
        return 5;
    }

    // A duplicate override is a diagnosed error, not silent last-wins.
    const std::vector<NodeIdInput> duplicate_override{
        button_input(42), make_input("ui/Counter.tsx", "Counter/root[1]", std::string(), 42)};
    if (!expect(is_error(assign_ui_ids("factory", duplicate_override),
                         IdErrorCode::DuplicateAutomationId, "duplicate automationId 42"),
                "duplicate override diagnosed") ||
        !expect(is_error(assign_ui_ids("factory", duplicate_override),
                         IdErrorCode::DuplicateAutomationId, "node 0"),
                "duplicate override names the first use")) {
        return 6;
    }

    // An explicit id that collides with another node's computed id is diagnosed too.
    const std::vector<NodeIdInput> override_hits_computed{
        button_input(), make_input("ui/Counter.tsx", "Counter/root[1]", std::string(), factory_id)};
    if (!expect(is_error(assign_ui_ids("factory", override_hits_computed),
                         IdErrorCode::DuplicateAutomationId, std::to_string(factory_id)),
                "override colliding with a computed id diagnosed")) {
        return 7;
    }

    // Two nodes with identical key inputs are a hash collision: a hard error, never renumbered.
    const std::vector<NodeIdInput> repeated{button_input(), button_input()};
    if (!expect(is_error(assign_ui_ids("factory", repeated), IdErrorCode::IdCollision,
                         std::to_string(factory_id)),
                "computed collision diagnosed")) {
        return 8;
    }

    // automationId 0 means "unset" in the scene; it cannot be an explicit id.
    const std::vector<NodeIdInput> zero_id{button_input(0)};
    if (!expect(is_error(assign_ui_ids("factory", zero_id), IdErrorCode::ZeroAutomationId),
                "zero automationId rejected")) {
        return 9;
    }

    // Input validation: the salt is required, and every node needs source + structural paths.
    const std::vector<NodeIdInput> one_node{button_input()};
    if (!expect(is_error(assign_ui_ids("", one_node), IdErrorCode::EmptySalt),
                "empty salt rejected") ||
        !expect(is_error(assign_ui_ids("factory", std::vector<NodeIdInput>{make_input(
                                                      "", "Counter/root[0]", std::string())}),
                         IdErrorCode::EmptySourcePath),
                "empty source path rejected") ||
        !expect(is_error(assign_ui_ids("factory", std::vector<NodeIdInput>{make_input(
                                                      "ui/Counter.tsx", "", std::string())}),
                         IdErrorCode::EmptyNodePath),
                "empty node path rejected")) {
        return 10;
    }

    // Repeated assignment is deterministic.
    if (!expect(assign_ui_ids("factory", defaults)->size() == default_result->size(),
                "repeat assignment size") ||
        !expect((*assign_ui_ids("factory", defaults))[1].ui_id == (*default_result)[1].ui_id,
                "repeat assignment values")) {
        return 11;
    }

    // Component ids use the same rule and the same collision policy. The duplicate and
    // missing-name inputs are named locals because GCC 15 warns when an inline temporary vector of
    // string-view inputs is passed straight into the assignment call.
    const std::vector<ComponentIdInput> components{{"ui/Counter.tsx", "Counter"},
                                                   {"ui/App.tsx", "App"}};
    const std::vector<ComponentIdInput> duplicate_components{{"ui/Counter.tsx", "Counter"},
                                                             {"ui/Counter.tsx", "Counter"}};
    const std::vector<ComponentIdInput> unnamed_component{{"ui/App.tsx", ""}};
    const auto component_result = assign_component_ids("factory", components);
    if (!expect(component_result.has_value(), "component assignment succeeds") ||
        !expect((*component_result)[0] == 15449676450390438563ULL, "computed component id") ||
        !expect((*component_result)[0] != (*component_result)[1], "components differ") ||
        !expect(is_error(assign_component_ids("factory", duplicate_components),
                         IdErrorCode::IdCollision, "id collision"),
                "duplicate component diagnosed") ||
        !expect(is_error(assign_component_ids("", components), IdErrorCode::EmptySalt),
                "component salt required") ||
        !expect(is_error(assign_component_ids("factory", unnamed_component),
                         IdErrorCode::EmptyComponentName),
                "component name required")) {
        return 12;
    }

    if (!expect(to_string(IdErrorCode::DuplicateAutomationId) == "duplicate_automation_id",
                "error code spelling") ||
        !expect(to_string(IdErrorCode::EmptySalt) == "empty_salt", "salt code spelling")) {
        return 13;
    }

    return 0;
}
