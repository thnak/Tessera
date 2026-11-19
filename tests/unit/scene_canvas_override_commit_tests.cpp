// SPDX-License-Identifier: MIT
//
// The commit half of the override layer (issue #536, designer-protocol-v1 sections 9.4 and 10.3):
// token-bound patches, the commit round trip and its mismatch, the layer's own history rules, and
// the report JSON. The set/restyle/undo half is scene_canvas_override_tests.cpp.

#include "scene_canvas_override_support.hpp"

namespace {

void test_token_bound_override_patches() {
    constexpr std::string_view kTokenCss = ".card { --accent: #3366ff; color: blue; }\n";
    constexpr std::string_view kTheme = R"({
  "schemaVersion": 1,
  "start": "light",
  "themes": {
    "light": {
      "--accent": "#3366ff"
    }
  }
})";
    Fixture fixture;
    cli::CanvasSessionSource source = cards_source();
    source.stylesheet = kTokenCss;
    expect(fixture.session.start(source).empty(), "the source starts");
    const std::vector<const ui::Node*> cards = cards_of(fixture.session);
    if (cards.size() != 2) {
        expect(false, "two cards");
        return;
    }
    const std::uint64_t id = cards[1]->automation_id();

    cli::PatchRequest request;
    request.automation_id = id;
    request.destination = cli::PatchDestination::Token;
    request.token = "--accent";
    request.token_value = "#ff0000";

    // No override: nothing to commit, whatever the destination.
    expect(fixture.session.patch(request).reason == cli::PatchErrorCode::NoOverrides,
           "a node with no overrides is no_overrides");

    fixture.session.set_theme_asset("assets.theme", std::string(kTheme), "light");
    expect(fixture.session.override_set(id, "color", "red").ok, "a literal override");
    expect(fixture.session.patch(request).reason == cli::PatchErrorCode::NotTokenBound,
           "a literal override is not token bound");

    expect(fixture.session.override_set(id, "color", "var(--accent)").ok, "a token override");
    const cli::SourcePatchResult result = fixture.session.patch(request);
    expect(result.ok, "a token-bound override makes the token patch succeed, got `" +
                          std::string(cli::patch_error_name(result.reason)) +
                          "`: " + result.detail);
    if (!result.ok || result.patch.edits.size() != 1) {
        return;
    }
    const cli::TextEdit& edit = result.patch.edits.front();
    expect(edit.file == "assets.theme" && kTheme.substr(edit.offset, edit.length) == "\"#3366ff\"",
           "the edit replaces the token's value in the theme asset");
    expect(!result.patch.base_versions.empty(), "the patch names the base versions");
}

constexpr std::string_view kPlain = R"tsx(import "./Preview.css";

export function Plain(): Node {
  return (
    <View className="card">
      <Text>Hello</Text>
    </View>
  );
}
)tsx";

// Applies the patch's edits in the order given, as a host does.
std::string apply_edits(std::string text, const std::vector<cli::TextEdit>& edits) {
    for (const cli::TextEdit& edit : edits) {
        text.replace(edit.offset, edit.length, edit.new_text);
    }
    return text;
}

void test_commit_round_trip() {
    cli::CanvasSceneSession session;
    cli::CanvasSessionSource source = source_for(kPlain, "ui/Plain.tsx");
    source.stylesheet = ".card { color: blue; }\n";
    expect(session.start(source).empty(), "the plain source starts");
    const ui::Node* card = find_class(*session.root(), "card");
    if (card == nullptr) {
        expect(false, "a card");
        return;
    }
    const std::uint64_t id = card->automation_id();
    expect(session.override_set(id, "padding-left", "8px").ok, "override");
    expect(session.override_set(id, "color", "green").ok, "override two");

    cli::PatchRequest request;
    request.automation_id = id;
    request.destination = cli::PatchDestination::Inline;
    const cli::SourcePatchResult patch = session.patch(request);
    expect(patch.ok, "the commit produces a patch: " + patch.detail);
    if (!patch.ok) {
        return;
    }
    // Both overrides travel together (section 10.1).
    const std::string patched = apply_edits(std::string(kPlain), patch.patch.edits);
    expect(patched.find("padding") != std::string::npos &&
               patched.find("green") != std::string::npos,
           "the patch writes every override of the node, got:\n" + patched);

    // The host applied it and the page evaluates the new text; then the commit settles.
    cli::CanvasSessionSource after = source;
    after.text = patched;
    expect(session.start(after).empty(), "the patched source evaluates: " + session.error());
    const cli::CommitSettleResult settled = session.settle_commit(id);
    expect(settled.ok && settled.consumed && settled.diagnostics.empty(),
           "the round trip gives the committed values, so the overrides are consumed");
    expect(session.overrides_of(id).empty(), "the layer no longer holds them");
    expect(!session.edit_undo().changed, "a commit is not undoable by edit.undo");
    const ui::Node* now = find_class(*session.root(), "card");
    expect(now != nullptr && session.inspect(now->automation_id()).overrides.empty(),
           "the inspector shows none");
}

void test_commit_mismatch_keeps_overrides() {
    cli::CanvasSceneSession session;
    cli::CanvasSessionSource source = source_for(kPlain, "ui/Plain.tsx");
    source.stylesheet = ".card { color: blue; }\n";
    expect(session.start(source).empty(), "starts");
    const ui::Node* card = find_class(*session.root(), "card");
    if (card == nullptr) {
        expect(false, "a card");
        return;
    }
    const std::uint64_t id = card->automation_id();
    expect(session.override_set(id, "padding-left", "8px").ok, "override");

    // The "committed" source is unchanged: it does NOT give the node the overridden style.
    expect(session.start(source).empty(), "the unchanged source evaluates");
    const cli::CommitSettleResult settled = session.settle_commit(id);
    expect(!settled.ok && !settled.consumed && settled.reason == "patch_mismatch",
           "a source that does not give the style is a mismatch");
    bool reported = false;
    for (const cli::OverrideDiagnostic& diagnostic : settled.diagnostics) {
        reported = reported || diagnostic.code == "TESSERA_DESIGNER_PATCH_MISMATCH";
    }
    expect(reported, "TESSERA_DESIGNER_PATCH_MISMATCH is reported");
    expect(session.overrides_of(id).size() == 1, "the override is kept so the developer can retry");
    expect(session.edit_undo().changed, "and its history is kept");
    expect(session.settle_commit(424242).reason == "no_such_node", "an unknown node");
}

void test_layer_history() {
    cli::OverrideLayer layer;
    // Node 1 and node 2 interleaved; committing node 1 must leave node 2's history intact and drop
    // the operations that only touched node 1.
    layer.set(1, "color", "red");
    layer.set(2, "color", "blue");
    layer.set(1, "padding-left", "4px");
    layer.set(2, "padding-left", "5px");
    layer.consume(1);
    expect(layer.of(1).empty() && layer.of(2).size() == 2, "consume removes only that node");
    int steps = 0;
    while (layer.undo()) {
        ++steps;
        expect(layer.of(1).empty(), "no undo step brings the consumed node back");
    }
    expect(steps == 2, "the two operations on node 1 collapsed, leaving node 2's two");
    expect(layer.empty(), "undone to the start");
    steps = 0;
    while (layer.redo()) {
        ++steps;
        expect(layer.of(1).empty(), "no redo step brings the consumed node back");
    }
    expect(steps == 2 && layer.of(2).size() == 2, "redo replays node 2's operations");
    // Property names are case-insensitive, like the cascade's.
    cli::OverrideLayer names;
    names.set(1, "Padding-Left", "1px");
    expect(!names.set(1, "padding-left", "1px"), "the same declaration in another case is a no-op");
}

void test_validate_override() {
    expect(!cli::validate_override("color", "#fff").has_value(), "a plain declaration is valid");
    expect(!cli::validate_override("--accent", "#fff").has_value(), "a custom property is valid");
    expect(!cli::validate_override("color", "var(--accent)").has_value(), "var() is valid");
    expect(cli::validate_override("", "red").has_value(), "no property");
    expect(cli::validate_override("color:red", "red").has_value(), "a colon in the name");
    expect(cli::validate_override("color", "red}").has_value(), "a closing brace");
    const auto unknown = cli::validate_override("flx-grow", "1");
    expect(unknown.has_value() && unknown->code == "TESSERA_CSS_UNKNOWN_PROPERTY",
           "a typo'd property is the compiler's own diagnostic");
}

void test_override_report_json() {
    Fixture fixture;
    if (!fixture.start()) {
        return;
    }
    const cli::OverrideOutcome ok = fixture.session.override_set(fixture.second, "color", "red");
    const std::string report = cli::canvas_override_report_json(ok);
    expect(report.starts_with("{\"overrideVersion\":1,\"ok\":true"), "the report is versioned");
    expect(report.find("\"overrides\":[{\"node\":\"" + std::to_string(fixture.second) +
                       "\",\"property\":\"color\",\"value\":\"red\"}]") != std::string::npos,
           "the node id is a decimal string: " + report);
    expect(report.back() == '\n' && report.find('\n') == report.size() - 1, "one line");
    const std::string refused = cli::canvas_override_report_json(
        fixture.session.override_set(fixture.second, "colour", "red"));
    expect(refused.find("\"ok\":false") != std::string::npos &&
               refused.find("\"code\":\"TESSERA_CSS_UNKNOWN_PROPERTY\"") != std::string::npos,
           "a refusal carries its diagnostic: " + refused);
    const std::string settle = cli::canvas_settle_report_json(cli::CommitSettleResult{});
    expect(settle.starts_with("{\"settleVersion\":1"), "the settle report is versioned");
}

} // namespace

int main() {
    test_token_bound_override_patches();
    test_commit_round_trip();
    test_commit_mismatch_keeps_overrides();
    test_layer_history();
    test_validate_override();
    test_override_report_json();
    if (failures() != 0) {
        std::cerr << "scene_canvas_override_commit: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_override_commit: ok\n";
    return 0;
}
