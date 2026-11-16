// SPDX-License-Identifier: MIT
//
// Pointer input into the canvas session (issue #388): the path from a pointer event to a painted
// `:hover`/`:active` look, and from a release to the generated `onClick`.
//
// Every clause of the acceptance sentence that does not need a browser is asserted here:
//
//  1. **`:hover` comes from the style engine.** The session writes no class and no colour: the
//     production `ui::InputRouter` sets `PseudoState::Hover` on the hit node and
//     `style::SceneStyleTree` resolves the selector against it. The test reads the *computed*
//     `background-color`, so a changed colour is evidence the cascade ran. The expected value is
//     the stylesheet's own `Button:hover` declaration, which is what makes this a test of the sheet
//     and not of a hard-coded constant in the session.
//  2. **`:active` likewise**, and it is a different colour from `:hover` - the two pseudo-classes
//  are
//     separate bits, not one "interacting" flag.
//  3. **Releasing over the button runs its `onClick` exactly once**, and the state it changed is in
//     the tree the next frame paints.
//  4. **Releasing elsewhere runs nothing** - click synthesis needs the press and release to share a
//     target, so a drag off the button is not a click.
//  5. **Every step is one on-demand frame**, counted by the harness: a move that stays on one node
//     costs none, and a leave costs one.

#include "scene_canvas_test_support.hpp"

namespace {

using namespace canvas_test;

void test_hover_through_the_style_engine() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "the source starts");
    const ui::Node* button = find_kind(*session.root(), "Button");
    expect(button != nullptr, "the source has a button");
    if (button == nullptr) {
        return;
    }
    const Point centre = centre_of(*button);
    const std::string resting = background_of(session, *button);
    expect(!resting.empty(), "the button has a resolved background-color at rest");

    CanvasHarness canvas;
    expect(canvas.ready(), "the harness has a device and a target");
    if (!canvas.ready()) {
        return;
    }
    expect(canvas.draw(session), "the Run's frame is painted before any input");
    expect(canvas.frames == 1, "one frame so far: the Run");

    // --- a move onto the button: :hover, and exactly one frame --------------------------------
    const cli::CanvasPointerResult hover =
        canvas.event(session, cli::PreviewInput::Kind::PointerMove, centre);
    expect(hover.dispatched, "the move reaches a scene target");
    expect(hover.changed, "a hover change owes a frame");
    expect(hover.hovered_id == button->automation_id(),
           "the hovered node is the button the pointer is over");
    expect(hover.active_id == 0, "a move sets no active state");
    expect(canvas.frames == 2, "the hover cost exactly one frame");

    const std::string hovered = background_of(session, *button);
    expect(hovered != resting, "the hovered button resolves a different background-color");
    // The stylesheet's own value: `Button:hover` declares #3d7def. Accepting the rgb() spelling too
    // keeps the assertion about the colour, not about how the cascade serializes it.
    expect(hovered == "#3d7def" || hovered == "rgb(61, 125, 239)",
           "the hover colour is the stylesheet's Button:hover value, got `" + hovered + "`");

    // --- a second move onto the same node: no frame -------------------------------------------
    // The clause that separates an on-demand canvas from an animation loop: a pointer that keeps
    // moving inside one button cannot make the page draw.
    const cli::CanvasPointerResult again = canvas.event(
        session, cli::PreviewInput::Kind::PointerMove, Point{centre.x + 2.0f, centre.y + 1.0f});
    expect(again.dispatched, "the second move is still dispatched");
    expect(!again.changed, "a move within the same hover target owes no frame");
    expect(canvas.frames == 2, "a move within one button painted nothing");

    // --- the leave: hover cleared, one frame ---------------------------------------------------
    const cli::CanvasPointerResult leave =
        canvas.event(session, cli::PreviewInput::Kind::PointerMove, kOutside);
    expect(leave.changed, "a move off the scene clears hover and owes a frame");
    expect(leave.hovered_id == 0, "nothing is hovered after the pointer leaves");
    expect(background_of(session, *button) == resting, "the button is back to its resting colour");
    expect(canvas.frames == 3, "the leave cost exactly one frame");

    // --- a second leave with nothing hovered: no frame -----------------------------------------
    const cli::CanvasPointerResult idle =
        canvas.event(session, cli::PreviewInput::Kind::PointerMove, kOutside);
    expect(!idle.changed, "a move outside with nothing hovered owes no frame");
    expect(!session.needs_frame(), "a static canvas reports no frame owed");
    expect(canvas.frames == 3, "the repeat leave painted nothing");
}

void test_press_release_runs_the_handler_once() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "the counter starts");
    const ui::Node* button = find_kind(*session.root(), "Button");
    if (button == nullptr) {
        expect(false, "the counter has a button");
        return;
    }
    const std::uint64_t button_id = button->automation_id();
    const Point centre = centre_of(*button);
    const std::string resting = background_of(session, *button);

    CanvasHarness canvas;
    expect(canvas.ready(), "the harness has a device and a target");
    if (!canvas.ready()) {
        return;
    }
    expect(canvas.draw(session), "the Run's frame is painted");

    (void)canvas.event(session, cli::PreviewInput::Kind::PointerMove, centre);
    const std::string hovered = background_of(session, *button);

    // --- press: :active, one frame -------------------------------------------------------------
    const cli::CanvasPointerResult press =
        canvas.event(session, cli::PreviewInput::Kind::PointerDown, centre);
    expect(press.dispatched, "the press reaches the button");
    expect(press.changed, "entering :active owes a frame");
    expect(press.active_id == button_id, "the pressed node carries :active");
    expect(press.hovered_id == button_id, "the pressed node stays hovered");
    expect(press.ran.empty(), "a press runs no click handler");
    const std::string active = background_of(session, *button);
    expect(active != hovered, ":active resolves a different colour than :hover");
    expect(active == "#1f4fae" || active == "rgb(31, 79, 174)",
           "the active colour is the stylesheet's Button:active value, got `" + active + "`");
    expect(canvas.frames == 3, "move + press cost exactly two frames after the Run");

    // --- release over the button: the handler runs, and its state is in the new tree ------------
    const cli::CanvasPointerResult release =
        canvas.event(session, cli::PreviewInput::Kind::PointerUp, centre);
    expect(release.dispatched, "the release is dispatched");
    expect(release.changed, "the click's state change owes a frame");
    expect(release.ran.size() == 1, "releasing over the button runs its onClick exactly once");
    expect(release.unrun.empty(), "the handler this slice cannot run stays empty");
    expect(canvas.frames == 4, "the release cost exactly one frame");
    expect(counter_value(session) == 1, "the click incremented the counter to 1");

    // The click rebuilt the tree, so `:active` is gone and the pointer must move again to re-hover.
    // Reported as a fact rather than hidden: a page that wants the hover back after a state change
    // has to send the move, and this test is where that requirement is written down.
    expect(session.router() != nullptr && session.router()->active() == nullptr,
           "the click cleared :active");
    const ui::Node* rebuilt = find_kind(*session.root(), "Button");
    expect(rebuilt != nullptr, "the rebuilt tree still has its button");
    if (rebuilt != nullptr) {
        expect(background_of(session, *rebuilt) == resting,
               "the rebuilt button starts at its resting colour until the next move");
    }
    // Nothing is owed after the click's frame: the canvas is static again.
    expect(!session.needs_frame() && !canvas.draw(session),
           "the clicked canvas is static again until the next event");
}

void test_release_elsewhere_does_not_run_the_handler() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kTwoButtons, "ui/TwoButtons.tsx")).empty(),
           "the two-button source starts");
    const std::vector<const ui::Node*> buttons = find_all(*session.root(), "Button");
    expect(buttons.size() == 2, "the source has two buttons");
    if (buttons.size() != 2) {
        return;
    }
    const Point first = centre_of(*buttons[0]);
    const Point second = centre_of(*buttons[1]);

    CanvasHarness canvas;
    if (!canvas.ready()) {
        expect(false, "the harness has a device and a target");
        return;
    }
    expect(canvas.draw(session), "the Run's frame is painted");
    const int baseline = canvas.frames;

    (void)canvas.event(session, cli::PreviewInput::Kind::PointerMove, first);
    const cli::CanvasPointerResult press =
        canvas.event(session, cli::PreviewInput::Kind::PointerDown, first);
    expect(press.active_id == buttons[0]->automation_id(), "the first button is pressed");
    // Released over the other button: click synthesis picks the nearest common ancestor, which is
    // the container, not the pressed button, so no `onClick` runs.
    const cli::CanvasPointerResult release =
        canvas.event(session, cli::PreviewInput::Kind::PointerUp, second);
    expect(release.dispatched, "the release is dispatched to the scene");
    expect(release.ran.empty(), "releasing over a different node runs no click handler");
    expect(counter_value(session) == 0,
           "the counter is still 0 after a click that missed its target");
    // The active state is cleared even though the press did not turn into a click: a pointer that
    // went up cannot leave a button stuck looking pressed.
    expect(session.router() != nullptr && session.router()->active() == nullptr,
           "the release cleared :active");
    // Move + press + release = three frames; a release that ran no handler costs one of them.
    expect(canvas.frames == baseline + 3, "each of the three events cost exactly one frame");
}

void test_secondary_button_is_ignored() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "the counter starts");
    const ui::Node* button = find_kind(*session.root(), "Button");
    if (button == nullptr) {
        expect(false, "the counter has a button");
        return;
    }
    const Point centre = centre_of(*button);

    CanvasHarness canvas;
    if (!canvas.ready()) {
        expect(false, "the harness has a device and a target");
        return;
    }
    expect(canvas.draw(session), "the Run's frame is painted");

    // Button 2 is the secondary mouse button. Dispatching it as a primary press would run an
    // `onClick` the user never asked for, so the session drops it and reports no change.
    const cli::CanvasPointerResult press =
        canvas.event(session, cli::PreviewInput::Kind::PointerDown, centre, 2);
    expect(!press.dispatched, "a secondary press is not dispatched");
    expect(!press.changed, "a secondary press owes no frame");
    const cli::CanvasPointerResult release =
        canvas.event(session, cli::PreviewInput::Kind::PointerUp, centre, 2);
    expect(!release.dispatched, "a secondary release is not dispatched");
    expect(counter_value(session) == 0, "a secondary press and release run no onClick");
    expect(canvas.frames == 1, "ignored buttons cost no frames");
}

void test_pointer_cancel_clears_active() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "the counter starts");
    const ui::Node* button = find_kind(*session.root(), "Button");
    if (button == nullptr) {
        expect(false, "the counter has a button");
        return;
    }
    const Point centre = centre_of(*button);

    CanvasHarness canvas;
    if (!canvas.ready()) {
        expect(false, "the harness has a device and a target");
        return;
    }
    expect(canvas.draw(session), "the Run's frame is painted");
    (void)canvas.event(session, cli::PreviewInput::Kind::PointerMove, centre);
    const cli::CanvasPointerResult press =
        canvas.event(session, cli::PreviewInput::Kind::PointerDown, centre);
    expect(press.active_id == button->automation_id(), "the button is pressed");

    // A cancel (the browser's `pointercancel`: the OS took the pointer away) must clear the pressed
    // look and run no handler.
    const cli::CanvasPointerResult cancel =
        canvas.event(session, cli::PreviewInput::Kind::PointerCancel, centre);
    expect(cancel.changed, "cancelling a press clears :active and owes a frame");
    expect(cancel.ran.empty(), "a cancel runs no click handler");
    expect(counter_value(session) == 0, "a cancelled press runs no onClick");
    expect(session.router() != nullptr && session.router()->active() == nullptr,
           "the cancel cleared :active");
}

} // namespace

int main() {
    test_hover_through_the_style_engine();
    test_press_release_runs_the_handler_once();
    test_release_elsewhere_does_not_run_the_handler();
    test_secondary_button_is_ignored();
    test_pointer_cancel_clears_active();
    if (failures() != 0) {
        std::cerr << "scene_canvas_pointer: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_pointer: ok\n";
    return 0;
}
