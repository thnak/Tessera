// SPDX-License-Identifier: MIT
//
// Shared harness for the wired-host tests: one real `app::AppHost` over the deterministic fixture
// scene, the concrete `EngineAutomationHost`, and small scene/snapshot helpers. Header-only with
// no shared state, so each test links its own copy.

#pragma once

#include "tessera/app/host.hpp"
#include "tessera/automation/engine_host.hpp"
#include "tessera/automation/fixture_scene.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::harness::testing {

// A windowless engine: root -> layout/style/paint host -> concrete automation host. Diagnostics are
// not attached by default; a test opts in through `automation.set_diagnostics`.
class HostFixture {
public:
    HostFixture()
        : host(root, sheet(), loop, config),
          fixture(root),
          automation(root, host, loop) {
        // One frame so layout bounds and hit testing are valid before the first command.
        (void)host.begin_frame(0.0);
        host.end_frame();
    }

    HostFixture(const HostFixture&) = delete;
    HostFixture& operator=(const HostFixture&) = delete;

    [[nodiscard]] static tessera::ui::style::CompiledStyleSheet sheet() {
        return tessera::ui::style::compile_stylesheet(
            tessera::automation::FixtureScene::stylesheet());
    }

    [[nodiscard]] static tessera::app::HostConfig make_config() {
        tessera::app::HostConfig value;
        value.width = 320.0f;
        value.height = 240.0f;
        value.dpi_scale = 1.0f;
        return value;
    }

    // Highest layout runs observed; the audit's layout check needs at least one pass.
    [[nodiscard]] std::size_t layout_runs() const {
        return host.layout_runs();
    }

    tessera::ui::Node root{1};
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop{clock};
    tessera::app::HostConfig config = make_config();
    tessera::app::AppHost host;
    tessera::automation::FixtureScene fixture;
    tessera::automation::EngineAutomationHost automation;
};

[[nodiscard]] inline tessera::ui::Node* find_node(tessera::ui::Node& root, std::uint64_t ui_id) {
    if (root.automation_id() == ui_id) {
        return &root;
    }
    for (const auto& child : root.children()) {
        if (tessera::ui::Node* found = find_node(*child, ui_id); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

// The node object of one snapshot entry, matched by its decimal `uiId`; nullptr when absent.
[[nodiscard]] inline const tessera::harness::JsonValue*
snapshot_node(const tessera::harness::JsonValue& snapshot, std::string_view ui_id) {
    const auto* nodes = snapshot.find("nodes");
    const auto* values = nodes == nullptr ? nullptr : nodes->as_array();
    if (values == nullptr) {
        return nullptr;
    }
    for (const tessera::harness::JsonValue& node : *values) {
        const auto* id = node.find("uiId");
        if (id != nullptr && id->as_string() == ui_id) {
            return &node;
        }
    }
    return nullptr;
}

} // namespace tessera::harness::testing
