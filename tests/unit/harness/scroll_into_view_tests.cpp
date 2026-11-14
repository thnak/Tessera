// SPDX-License-Identifier: MIT
//
// `scrollIntoView` with an `itemIndex` reveals an unrealized item in a manifest virtual
// collection (`docs/specs/headless-automation-v1.md` section 6.8): the host resolves the
// collection, `VirtualRange`/`VirtualGridRows` realize the window, and the realized item becomes
// addressable by `(uiId, itemKey)` in the same script. The node-targeting form is unchanged.

#include "engine_host_support.hpp"
#include "tessera/automation/manifest_scene.hpp"
#include "tessera/diagnostics/trace.hpp"
#include "tessera/harness/command.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/script.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>

namespace {

namespace automation = tessera::automation;
namespace harness = tessera::harness;
namespace testing = tessera::harness::testing;
namespace ui = tessera::ui;

using tessera::app::AppHost;
using tessera::app::HostConfig;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_scroll_into_view: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool near(float value, float expected, float tolerance = 1.0e-4f) {
    return std::fabs(value - expected) <= tolerance;
}

// Manifest scene ids: a root View plus one list, one grid and one plain Button.
constexpr std::uint64_t kViewId = 7000000000000000101ULL;
constexpr std::uint64_t kListId = 7000000000000000102ULL;
constexpr std::uint64_t kGridId = 7000000000000000103ULL;
constexpr std::uint64_t kButtonId = 7000000000000000104ULL;

automation::AppManifest make_manifest() {
    automation::AppManifest manifest;
    manifest.project_salt = "scroll-into-view";
    manifest.nodes = {
        automation::ManifestNode{kViewId, 1, "View", "App/root[0]", "", "", 0, 0},
        automation::ManifestNode{kListId, 1, "VirtualList", "App/root[0]/VirtualList[0]", "", "", 0,
                                 0},
        automation::ManifestNode{kGridId, 1, "VirtualGrid", "App/root[0]/VirtualGrid[1]", "", "", 0,
                                 0},
        automation::ManifestNode{kButtonId, 1, "Button", "App/root[0]/Button[2]", "", "", 0, 0},
    };
    return manifest;
}

HostConfig host_config() {
    HostConfig config;
    config.width = 320.0f;
    config.height = 240.0f;
    config.dpi_scale = 1.0f;
    return config;
}

automation::EngineAutomationHost::Config
make_automation_config(automation::VirtualCollectionRegistry& registry) {
    automation::EngineAutomationHost::Config value;
    value.virtual_collections = &registry;
    return value;
}

// Bare Node -> ManifestScene -> AppHost -> EngineAutomationHost, with the scene's live virtual
// collections registered on the host.
struct ManifestFixture {
    ui::Node root{1};
    ManualClock clock;
    RuntimeLoop loop{clock};
    HostConfig config = host_config();
    automation::AppManifest manifest = make_manifest();
    automation::ManifestScene scene{root, manifest};
    automation::VirtualCollectionRegistry& registry = scene.virtual_collections();
    AppHost host{root,
                 ui::style::compile_stylesheet(automation::ManifestScene::stylesheet(manifest)),
                 loop, config};
    automation::EngineAutomationHost automation{root, host, loop, make_automation_config(registry)};

    ManifestFixture() {
        (void)host.begin_frame(0.0);
        host.end_frame();
    }

    ManifestFixture(const ManifestFixture&) = delete;
    ManifestFixture& operator=(const ManifestFixture&) = delete;
};

automation::VirtualCollection* find_collection(ManifestFixture& fixture, std::uint64_t ui_id) {
    ui::Node* node = testing::find_node(fixture.root, ui_id);
    return node == nullptr ? nullptr : fixture.scene.virtual_collections().find(*node);
}

harness::ActionCommand index_action(
    harness::ActionKind kind, std::uint64_t ui_id, std::size_t index,
    harness::ScrollIntoViewAlignment alignment = harness::ScrollIntoViewAlignment::Nearest) {
    harness::ActionCommand command;
    command.kind = kind;
    command.ui_id = ui_id;
    command.item_index = index;
    command.alignment = alignment;
    return command;
}

harness::ActionCommand key_action(harness::ActionKind kind, std::uint64_t ui_id,
                                  std::string_view key) {
    harness::ActionCommand command;
    command.kind = kind;
    command.ui_id = ui_id;
    command.item_key = std::string(key);
    return command;
}

// Snapshot entry for one realized item: matches the automation id and the item key together,
// because the collection container carries the same uiId without a key.
const harness::JsonValue* snapshot_item(const harness::JsonValue& snapshot, std::string_view ui_id,
                                        std::string_view key) {
    const auto* nodes = snapshot.find("nodes");
    const auto* values = nodes == nullptr ? nullptr : nodes->as_array();
    if (values == nullptr) {
        return nullptr;
    }
    for (const harness::JsonValue& node : *values) {
        const auto* id = node.find("uiId");
        const auto* item_key = node.find("itemKey");
        if (id != nullptr && item_key != nullptr && id->as_string() == ui_id &&
            item_key->as_string() == key) {
            return &node;
        }
    }
    return nullptr;
}

void reveals_unrealized_item() {
    tessera::diagnostics::TraceRecorder recorder{256};
    ManifestFixture fixture;
    automation::AutomationDiagnostics hooks;
    hooks.trace = &recorder;
    hooks.enabled = true;
    fixture.automation.set_diagnostics(hooks);

    const auto before = fixture.automation.canonical_snapshot(false);
    if (!expect(before.has_value(), "the pre-scroll snapshot succeeds")) {
        return;
    }
    expect(snapshot_item(*before, "7000000000000000102", "40") == nullptr,
           "item 40 is not realized before the scroll");
    expect(snapshot_item(*before, "7000000000000000102", "0") != nullptr,
           "the initial window realizes item 0");

    auto reveal = fixture.automation.dispatch_action(
        index_action(harness::ActionKind::ScrollIntoView, kListId, 40));
    expect(reveal.has_value(), "indexed scrollIntoView succeeds");

    automation::VirtualCollection* list = find_collection(fixture, kListId);
    ui::Node* realized = list == nullptr ? nullptr : list->realized_node(40);
    if (!expect(realized != nullptr, "item 40 is realized")) {
        return;
    }
    expect(realized->automation_id() == kListId, "the realized item carries the collection uiId");
    expect(realized->item_key() == 40, "the realized item carries its key");
    expect(realized->id() != 0, "the realized item has a unique runtime id");
    expect(near(realized->world_bounds().height, 16.0f), "the realized item has its item extent");

    const auto after = fixture.automation.canonical_snapshot(false);
    const auto* node =
        after.has_value() ? snapshot_item(*after, "7000000000000000102", "40") : nullptr;
    if (!expect(node != nullptr, "the post-scroll snapshot shows item 40")) {
        return;
    }
    const auto* bounds = node->find("bounds");
    const auto* height = bounds == nullptr ? nullptr : bounds->find("height");
    expect(height != nullptr && height->as_number().has_value() &&
               near(static_cast<float>(*height->as_number()), 16.0f),
           "the snapshot item has the realized height");

#if defined(TESSERA_ENABLE_TRACING)
    const auto trace = fixture.automation.collect_trace();
    expect(trace.has_value() && trace->json.find("automation.scrollIntoView") != std::string::npos,
           "the reveal records an automation.scrollIntoView span");
#endif

    // A follow-up tap proves the item is addressable, not merely present in the window.
    auto tap =
        fixture.automation.dispatch_action(key_action(harness::ActionKind::Click, kListId, "40"));
    expect(tap.has_value(), "the realized item is tappable by (uiId, itemKey)");
    expect(fixture.automation.input().router().focus_manager().focused() == realized,
           "the tap focuses the realized item");
}

void alignment_offsets() {
    struct Case {
        harness::ScrollIntoViewAlignment alignment;
        float offset;
        float top;
        const char* label;
    };
    const Case cases[] = {
        {harness::ScrollIntoViewAlignment::Start, 640.0f, 0.0f, "Start"},
        {harness::ScrollIntoViewAlignment::Center, 608.0f, 32.0f, "Center"},
        {harness::ScrollIntoViewAlignment::End, 576.0f, 64.0f, "End"},
        {harness::ScrollIntoViewAlignment::Nearest, 576.0f, 64.0f, "Nearest"},
    };
    for (const Case& item : cases) {
        ManifestFixture fixture;
        std::string failure = std::string("alignment ") + item.label;
        auto reveal = fixture.automation.dispatch_action(
            index_action(harness::ActionKind::ScrollIntoView, kListId, 40, item.alignment));
        automation::VirtualCollection* list = find_collection(fixture, kListId);
        ui::Node* realized = list == nullptr ? nullptr : list->realized_node(40);
        if (!expect(reveal.has_value() && list != nullptr && realized != nullptr,
                    failure.c_str())) {
            continue;
        }
        expect(near(list->scroll_offset(), item.offset), failure.c_str());
        expect(near(realized->bounds().y, item.top), failure.c_str());
    }
}

void grid_later_row() {
    ManifestFixture fixture;
    auto reveal = fixture.automation.dispatch_action(index_action(
        harness::ActionKind::ScrollIntoView, kGridId, 42, harness::ScrollIntoViewAlignment::Start));
    expect(reveal.has_value(), "grid scrollIntoView succeeds");

    automation::VirtualCollection* grid = find_collection(fixture, kGridId);
    ui::Node* realized = grid == nullptr ? nullptr : grid->realized_node(42);
    if (!expect(grid != nullptr && realized != nullptr, "grid item 42 is realized")) {
        return;
    }
    // Four columns over the 200 px container: row 10 starts at 160 px, cell width 50.
    expect(near(grid->scroll_offset(), 160.0f), "the grid scrolls to row 10");
    expect(near(realized->bounds().x, 100.0f), "item 42 sits in column 2");
    expect(near(realized->bounds().y, 0.0f), "the target row is at the viewport start");
    expect(near(realized->bounds().width, 50.0f), "the grid cell uses a quarter of the width");
    expect(near(realized->bounds().height, 16.0f), "the grid row extent is 16 px");

    const auto snapshot = fixture.automation.canonical_snapshot(false);
    expect(snapshot.has_value() && snapshot_item(*snapshot, "7000000000000000103", "42") != nullptr,
           "the snapshot shows the grid item");
    auto tap =
        fixture.automation.dispatch_action(key_action(harness::ActionKind::Click, kGridId, "42"));
    expect(tap.has_value(), "the grid item is tappable by (uiId, itemKey)");
}

void error_paths() {
    ManifestFixture fixture;
    auto out_of_range = fixture.automation.dispatch_action(
        index_action(harness::ActionKind::ScrollIntoView, kListId, 100));
    expect(!out_of_range.has_value() &&
               out_of_range.error().code == harness::ProtocolErrorCode::InvalidArgument,
           "an out-of-range index is invalidArgument");
    expect(!out_of_range.has_value() &&
               out_of_range.error().message.find("100") != std::string::npos,
           "the out-of-range message names the index and the count");
    expect(!out_of_range.has_value() && out_of_range.error().detail == "100/100",
           "the out-of-range detail is index/count");

    auto plain = fixture.automation.dispatch_action(
        index_action(harness::ActionKind::ScrollIntoView, kButtonId, 0));
    expect(!plain.has_value() && plain.error().code == harness::ProtocolErrorCode::UiIdNotFound,
           "a non-collection target is uiIdNotFound");
    expect(!plain.has_value() &&
               plain.error().message.find("virtual collection") != std::string::npos,
           "the non-collection message names the missing seam");
    expect(!out_of_range.has_value() && !plain.has_value() &&
               out_of_range.error().code != plain.error().code,
           "the two failures are distinct typed errors");

    auto unknown = fixture.automation.dispatch_action(
        index_action(harness::ActionKind::ScrollIntoView, 9999, 0));
    expect(!unknown.has_value() && unknown.error().code == harness::ProtocolErrorCode::UiIdNotFound,
           "an unknown collection uiId is uiIdNotFound");
}

void command_and_script_round_trip() {
    const auto parsed = harness::parse_json(
        R"({"uiId":"7000000000000000102","itemIndex":40,"alignment":"center"})");
    if (!expect(parsed.has_value(), "the command arguments parse")) {
        return;
    }
    const auto decoded = harness::decode_command("scrollIntoView", *parsed);
    if (!expect(decoded.has_value(), "the indexed action decodes")) {
        return;
    }
    const auto* action = std::get_if<harness::ActionCommand>(&*decoded);
    if (!expect(action != nullptr && action->item_index == 40 &&
                    action->alignment == harness::ScrollIntoViewAlignment::Center,
                "itemIndex and alignment decode")) {
        return;
    }
    const auto encoded = harness::encode_command_arguments(*decoded);
    expect(encoded.has_value() && encoded->find("itemIndex") != nullptr &&
               encoded->find("itemIndex")->as_integer() == 40 &&
               encoded->find("alignment")->as_string() == "center",
           "the indexed action re-encodes canonically");
    if (encoded.has_value()) {
        const auto redecoded = harness::decode_command("scrollIntoView", *encoded);
        expect(redecoded.has_value() && std::get<harness::ActionCommand>(*redecoded) == *action,
               "decode(encode(x)) round-trips");
    }

    for (const char* bad : {R"({"uiId":"1","itemIndex":40,"itemKey":"40"})",
                            R"({"uiId":"1","alignment":"end"})", R"({"uiId":"1","itemIndex":-1})",
                            R"({"uiId":"1","itemIndex":40,"alignment":"middle"})"}) {
        const auto args = harness::parse_json(bad);
        if (!args.has_value()) {
            continue;
        }
        const auto result = harness::decode_command("scrollIntoView", *args);
        expect(!result.has_value() &&
                   result.error().code == harness::ProtocolErrorCode::InvalidArgument,
               "an invalid item field is invalidArgument");
    }

    const auto script = harness::parse_script(
        R"({"scriptVersion":1,"steps":[{"action":"scrollIntoView","uiId":"7000000000000000102","itemIndex":40,"alignment":"end"}]})");
    expect(script.has_value() && script->steps.size() == 1 && script->steps[0].item_index == 40 &&
               script->steps[0].alignment == harness::ScrollIntoViewAlignment::End,
           "a harness script carries itemIndex and alignment");
}

void node_targeting_is_unchanged() {
    testing::HostFixture fixture;
    ui::Node* list = testing::find_node(fixture.root, automation::kFixtureListId);
    if (!expect(list != nullptr, "the fixture list exists")) {
        return;
    }
    ui::Node* row = nullptr;
    for (const auto& child : list->children()) {
        if (child->item_key() == 101) {
            row = child.get();
        }
    }
    if (!expect(row != nullptr, "the fixture row 101 exists")) {
        return;
    }
    (void)ui::scroll_to(*list, 0.0f, ui::max_scroll_y(*list));
    const float scrolled_away = list->scroll_y();

    auto reveal = fixture.automation.dispatch_action(
        key_action(harness::ActionKind::ScrollIntoView, automation::kFixtureRowId, "101"));
    expect(reveal.has_value(), "node-targeted scrollIntoView still succeeds");
    expect(list->scroll_y() < scrolled_away, "node-targeted scrollIntoView still reveals the row");
    expect(row->world_bounds().y >= list->world_bounds().y, "the row is inside the scrollport");

    // Without a registry an index is a non-collection target, never a silent node action.
    auto indexed = fixture.automation.dispatch_action(
        index_action(harness::ActionKind::ScrollIntoView, automation::kFixtureListId, 0));
    expect(!indexed.has_value() && indexed.error().code == harness::ProtocolErrorCode::UiIdNotFound,
           "an index without a virtual collection is uiIdNotFound");
}

} // namespace

int main() {
    reveals_unrealized_item();
    alignment_offsets();
    grid_later_row();
    error_paths();
    command_and_script_round_trip();
    node_targeting_is_unchanged();
    return failures == 0 ? 0 : 1;
}
