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

#include "scene_canvas_override_support.hpp"

namespace {

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

} // namespace

int main() {
    test_set_restyles_without_evaluating();
    test_invalid_value_is_refused();
    test_override_wins_and_is_per_node();
    test_clear_and_revert();
    test_override_survives_a_rebuild();
    test_undo_redo();
    if (failures() != 0) {
        std::cerr << "scene_canvas_override: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_override: ok\n";
    return 0;
}
