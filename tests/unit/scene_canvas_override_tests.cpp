// SPDX-License-Identifier: MIT
//
// The designer's override layer (issue #536, designer-protocol-v1 sections 9.2-9.4 and 10.3): the
// input `source.patch` needs, and the missing half of designer.md section 8.2's loop.
//
// The acceptance sentences, and where each is asserted:
//
//  1. A valid override changes the node's computed value and owes the next frame, without
//     re-evaluating the source: `test_set_restyles_without_evaluating`.
//  2. An invalid value is refused with its `TESSERA_CSS_*` diagnostic and leaves the node
//     unchanged: `test_invalid_value_is_refused`.
//  3. An override beats the author and inline origins, touches only its own node, and clear/revert
//     take it off again: `test_override_wins_and_is_per_node`, `test_clear_and_revert`.
//  4. Overrides survive the rebuild a state change causes: `test_override_survives_a_rebuild`.
//  5. Undo/redo of set/clear/revert, with a new operation cutting the redo branch:
//     `test_undo_redo`.
//  6. A `token`-bound override makes a `token` patch succeed: `test_token_bound_override_patches`.
//  7. A commit carries the node's overrides and the round trip clears them; a source that does not
//     give the committed style reports `TESSERA_DESIGNER_PATCH_MISMATCH` and keeps them:
//     `test_commit_round_trip`, `test_commit_mismatch_keeps_overrides`.
//  8. The layer's own history rules, with no tree: `test_layer_history`, `test_validate_override`.
//
// Every computed value is read through `SceneStyleTree::compute()` over the session's own sheet,
// the call the paint pass makes, so a passing assertion is evidence about what is painted. No wall
// clock and no browser.

#include "scene_canvas_test_support.hpp"
#include "scene_override.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace canvas_test;

// Two `.card` nodes (one with an inline colour, to prove an override beats the inline origin) and a
// button that changes state, which rebuilds the tree.
constexpr std::string_view kCards = R"tsx(import "./Preview.css";

export function Cards(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <View className="card" style={{ color: "red" }}>
        <Text>One</Text>
      </View>
      <View className="card">
        <Text>Two</Text>
      </View>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
)tsx";

constexpr std::string_view kCardsCss = ".card { color: blue; padding-left: 2px; }\n";

cli::CanvasSessionSource cards_source() {
    cli::CanvasSessionSource source = source_for(kCards, "ui/Cards.tsx");
    source.stylesheet = kCardsCss;
    source.stylesheet_file = "ui/Preview.css";
    return source;
}

void collect_class(const ui::Node& node, std::string_view class_name,
                   std::vector<const ui::Node*>& out) {
    if (node.class_name() == class_name) {
        out.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_class(*child, class_name, out);
    }
}

std::vector<const ui::Node*> cards_of(const cli::CanvasSceneSession& session) {
    std::vector<const ui::Node*> cards;
    collect_class(*session.root(), "card", cards);
    return cards;
}

std::string computed(const cli::CanvasSceneSession& session, std::uint64_t automation_id,
                     std::string_view property) {
    ui::style::SceneStyleTree tree(*session.root());
    for (const ui::Node* node : cards_of(session)) {
        if (node->automation_id() == automation_id) {
            const ui::style::ComputedStyle style = tree.compute(session.sheet(), *node);
            const std::string* value = style.find(property);
            return value != nullptr ? *value : std::string();
        }
    }
    return {};
}

bool has_code(const cli::OverrideOutcome& outcome, std::string_view code) {
    for (const cli::OverrideDiagnostic& diagnostic : outcome.diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

// Starts the two-card source and returns the two cards' automation ids.
struct Fixture {
    cli::CanvasSceneSession session;
    std::uint64_t first = 0;
    std::uint64_t second = 0;

    bool start() {
        const std::string error = session.start(cards_source());
        expect(error.empty(), "the cards source starts: " + error);
        const std::vector<const ui::Node*> cards = cards_of(session);
        expect(cards.size() == 2, "the source has two .card nodes");
        if (!error.empty() || cards.size() != 2) {
            return false;
        }
        first = cards[0]->automation_id();
        second = cards[1]->automation_id();
        return true;
    }
};

void test_set_restyles_without_evaluating() {
    Fixture fixture;
    if (!fixture.start()) {
        return;
    }
    cli::CanvasSceneSession& session = fixture.session;
    CanvasHarness harness;
    expect(harness.draw(session), "the first run paints");
    expect(!session.needs_frame(), "a painted static scene owes nothing");
    const ui::Node* root_before = session.root();
    const std::uint64_t generation = session.cascade_generation();

    const cli::OverrideOutcome outcome = session.override_set(fixture.second, "color", "green");
    expect(outcome.ok && outcome.changed, "a valid override is accepted and changes the layer");
    expect(outcome.diagnostics.empty(), "a valid override carries no diagnostic");
    expect(outcome.overrides.size() == 1 && outcome.overrides[0].property == "color" &&
               outcome.overrides[0].value == "green" &&
               outcome.overrides[0].automation_id == fixture.second,
           "the answer lists the node's overrides");
    expect(computed(session, fixture.second, "color") != "blue" &&
               !computed(session, fixture.second, "color").empty(),
           "the node's computed colour is no longer the author's");
    expect(session.needs_frame(), "an override owes the next frame");
    expect(harness.draw(session), "the frame is painted");
    expect(!session.needs_frame(), "one frame is enough");
    expect(session.root() == root_before && session.cascade_generation() == generation,
           "the tree was restyled in place, not evaluated again");

    // The inspector lists the override and its computed value is the overridden one.
    const cli::StyleInfoResult info = session.inspect(fixture.second);
    expect(info.overrides.size() == 1 && info.overrides[0].value == "green",
           "style.info lists the node's override");

    // Setting the identical value again is a no-op, not a second history entry and not a frame.
    const cli::OverrideOutcome again = session.override_set(fixture.second, "color", "green");
    expect(again.ok && !again.changed, "the same value twice changes nothing");
    expect(!session.needs_frame(), "the no-op owes no frame");
}

void test_invalid_value_is_refused() {
    Fixture fixture;
    if (!fixture.start()) {
        return;
    }
    cli::CanvasSceneSession& session = fixture.session;
    CanvasHarness harness;
    (void)harness.draw(session);
    const std::string before = computed(session, fixture.second, "padding-left");

    const cli::OverrideOutcome unknown = session.override_set(fixture.second, "colour", "red");
    expect(!unknown.ok && has_code(unknown, "TESSERA_CSS_UNKNOWN_PROPERTY"),
           "an unknown property is refused with TESSERA_CSS_UNKNOWN_PROPERTY");
    const cli::OverrideOutcome invalid =
        session.override_set(fixture.second, "padding", "1 2 3 4 5");
    expect(!invalid.ok && has_code(invalid, "TESSERA_CSS_INVALID_VALUE"),
           "a shorthand that does not expand is refused with TESSERA_CSS_INVALID_VALUE");
    const cli::OverrideOutcome injected =
        session.override_set(fixture.second, "color", "red; background-color: blue");
    expect(!injected.ok && has_code(injected, "TESSERA_CSS_PARSE_ERROR"),
           "a value that is two declarations is refused");
    const cli::OverrideOutcome empty = session.override_set(fixture.second, "color", "  ");
    expect(!empty.ok && has_code(empty, "TESSERA_CSS_PARSE_ERROR"), "an empty value is refused");
    const cli::OverrideOutcome huge =
        session.override_set(fixture.second, "color", std::string(300, 'a'));
    expect(!huge.ok && has_code(huge, "TESSERA_DESIGNER_LIMIT"),
           "a value the scene cannot hold is refused, not truncated");
    const cli::OverrideOutcome missing = session.override_set(999999, "color", "red");
    expect(!missing.ok && missing.reason == "no_such_node", "an unknown NodeRef is no_such_node");

    expect(session.overrides_of(fixture.second).empty(), "no refusal left an override behind");
    expect(computed(session, fixture.second, "padding-left") == before, "the node is unchanged");
    expect(!session.needs_frame(), "a refusal owes no frame");
    expect(!session.edit_undo().changed, "a refusal is not a history entry");
}

void test_override_wins_and_is_per_node() {
    Fixture fixture;
    if (!fixture.start()) {
        return;
    }
    cli::CanvasSceneSession& session = fixture.session;
    const std::string first_before = computed(session, fixture.first, "color");
    const std::string second_before = computed(session, fixture.second, "color");
    expect(first_before != second_before,
           "the inline red beats the author blue before any override");

    expect(session.override_set(fixture.first, "color", "green").ok,
           "override the inline-styled node");
    expect(computed(session, fixture.first, "color") != first_before,
           "an override beats the inline origin");
    expect(computed(session, fixture.second, "color") == second_before,
           "the other instance of the component is untouched");

    // A shorthand is applied as its longhands, by the registry's own expansion.
    expect(session.override_set(fixture.second, "padding", "9px").ok, "a shorthand override");
    expect(computed(session, fixture.second, "padding-left") == "9px",
           "the shorthand reaches the longhand the layout reads");
    expect(computed(session, fixture.first, "padding-left") != "9px", "and only on its node");
}

void test_clear_and_revert() {
    Fixture fixture;
    if (!fixture.start()) {
        return;
    }
    cli::CanvasSceneSession& session = fixture.session;
    const std::string first_before = computed(session, fixture.first, "color");
    expect(session.override_set(fixture.first, "color", "green").ok, "set color");
    expect(session.override_set(fixture.first, "padding-left", "7px").ok, "set padding");
    expect(session.override_set(fixture.second, "color", "green").ok, "set on the second node");

    const cli::OverrideOutcome one = session.override_clear(fixture.first, "color");
    expect(one.ok && one.changed && one.overrides.size() == 1, "clear removes one property");
    expect(computed(session, fixture.first, "color") == first_before,
           "clearing restores the inline value it replaced, not a blank");
    expect(computed(session, fixture.first, "padding-left") == "7px",
           "the node's other override stays");

    const cli::OverrideOutcome all = session.override_clear(fixture.first);
    expect(all.ok && all.overrides.empty(), "clear with no property removes all of the node's");
    expect(computed(session, fixture.first, "padding-left") != "7px", "and restyles");
    expect(session.overrides_of(fixture.second).size() == 1, "the other node keeps its override");
    expect(!session.override_clear(fixture.first).changed, "clearing nothing is a no-op");

    const cli::OverrideOutcome reverted = session.override_revert();
    expect(reverted.ok && reverted.changed && reverted.overrides.empty(),
           "revert removes every override in the viewer");
    expect(computed(session, fixture.second, "color") != "green" &&
               computed(session, fixture.second, "color") ==
                   computed(session, fixture.second, "color"),
           "revert restyles the second node");
    expect(session.override_clear(999999).reason == "no_such_node", "clear of an unknown node");
}

void test_override_survives_a_rebuild() {
    Fixture fixture;
    if (!fixture.start()) {
        return;
    }
    cli::CanvasSceneSession& session = fixture.session;
    expect(session.override_set(fixture.second, "padding-left", "11px").ok, "set");
    const ui::Node* button = find_kind(*session.root(), "Button");
    expect(button != nullptr, "the source has a button");
    if (button == nullptr) {
        return;
    }
    const Point at = centre_of(*button);
    (void)session.pointer(cli::PreviewInput::Kind::PointerMove, at.x, at.y);
    (void)session.pointer(cli::PreviewInput::Kind::PointerDown, at.x, at.y);
    (void)session.pointer(cli::PreviewInput::Kind::PointerUp, at.x, at.y);
    expect(counter_value(session) == 1, "the click changed state, which rebuilds the tree");
    expect(computed(session, fixture.second, "padding-left") == "11px",
           "the override is applied to the rebuilt tree");
    // And a re-evaluation of changed text re-applies it too.
    cli::CanvasSessionSource edited = cards_source();
    std::string text(kCards);
    text.replace(text.find("Two"), 3, "Deux");
    edited.text = text;
    expect(session.start(edited).empty(), "an edited source evaluates");
    expect(computed(session, fixture.second, "padding-left") == "11px",
           "the override survives a re-evaluation");
}

void test_undo_redo() {
    Fixture fixture;
    if (!fixture.start()) {
        return;
    }
    cli::CanvasSceneSession& session = fixture.session;
    const std::string base = computed(session, fixture.second, "padding-left");
    (void)session.override_set(fixture.second, "padding-left", "5px");
    (void)session.override_set(fixture.second, "padding-left", "6px");
    (void)session.override_clear(fixture.second);
    expect(computed(session, fixture.second, "padding-left") == base, "cleared");

    expect(session.edit_undo().changed, "undo of the clear");
    expect(computed(session, fixture.second, "padding-left") == "6px", "the clear is undone");
    expect(session.edit_undo().changed, "undo of the second set");
    expect(computed(session, fixture.second, "padding-left") == "5px", "the second set is undone");
    const cli::OverrideOutcome redo = session.edit_redo();
    expect(redo.changed && redo.can_redo && redo.can_undo, "redo");
    expect(computed(session, fixture.second, "padding-left") == "6px", "redo re-applies");

    // A new operation cuts the redo branch.
    (void)session.override_set(fixture.second, "color", "green");
    expect(!session.edit_redo().changed, "a new operation clears redo");

    // `revert` is an operation like the others.
    (void)session.override_revert();
    expect(session.overrides_of(fixture.second).empty(), "reverted");
    expect(session.edit_undo().changed && session.overrides_of(fixture.second).size() == 2,
           "undo of a revert brings every override back");
    // Undo past the start of the history is a no-op.
    while (session.edit_undo().changed) {
    }
    expect(computed(session, fixture.second, "padding-left") == base, "all the way back");
    expect(!session.edit_undo().can_undo, "nothing left to undo");
}

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
    test_set_restyles_without_evaluating();
    test_invalid_value_is_refused();
    test_override_wins_and_is_per_node();
    test_clear_and_revert();
    test_override_survives_a_rebuild();
    test_undo_redo();
    test_token_bound_override_patches();
    test_commit_round_trip();
    test_commit_mismatch_keeps_overrides();
    test_layer_history();
    test_validate_override();
    test_override_report_json();
    if (failures() != 0) {
        std::cerr << "scene_canvas_override: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_override: ok\n";
    return 0;
}
