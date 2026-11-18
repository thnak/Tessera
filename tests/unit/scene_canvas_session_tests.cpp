// SPDX-License-Identifier: MIT
//
// The canvas module's persistent session (issue #388): one evaluation that pointer events feed and
// that a frame paints, plus the on-demand frame accounting that decides whether anything is drawn.
//
// The pointer path itself is covered by `scene_canvas_pointer_tests.cpp`; this file covers the
// session's lifecycle and the frame contract. Both share `scene_canvas_test_support.hpp`.
//
// Frame counting is done at the two layers that own the decision: `needs_frame()` says whether a
// frame is owed, and the null RHI backend records whether one was actually painted. A "frame" in
// these assertions is therefore a recorded command stream, not a mocked bool.
//
//  1. A Run paints exactly one frame and then the canvas is static.
//  2. A static scene presents nothing: `draw()` on an unchanged session paints zero frames.
//  3. State and the tree survive a Run of the same source, and state crosses an edit with the P4
//     carry rule.
//  4. A source that does not evaluate reports a reason and paints nothing.
//  5. An edit that does not evaluate keeps the running evaluation, its state and its picture
//     (issue #444: the browser applies every keystroke, so a half-typed source must cost a
//     diagnostic only, never the frame).

#include "scene_canvas_test_support.hpp"

#include <string>

namespace {

using namespace canvas_test;

namespace css = tessera::compiler::css_module;

void test_start_and_first_frame() {
    cli::CanvasSceneSession session;
    expect(!session.started(), "a fresh session is not started");
    expect(!session.needs_frame(), "an unstarted session owes no frame");

    const std::string error = session.start(source_for(kCounter, "ui/Counter.tsx"));
    expect(error.empty(), "the counter source starts: " + error);
    expect(session.started(), "the session is started after a successful start");
    expect(session.root() != nullptr, "a started session has a tree");
    // A Run is a change: the page draws once. That is the only frame a freshly opened canvas costs.
    expect(session.needs_frame(), "a fresh evaluation owes exactly one frame");
    expect(session.frames_painted() == 0, "nothing has been painted yet");

    CanvasHarness canvas;
    expect(canvas.ready(), "the null backend creates the frame's target");
    if (!canvas.ready()) {
        return;
    }
    expect(canvas.draw(session), "the first frame is painted");
    expect(session.frames_painted() == 1, "the session counted one painted frame");
    expect(canvas.last_draws >= 1, "the first frame records at least one draw call");
    expect(canvas.last_nodes >= 3, "the frame reports the painted nodes");
    // The painted frame consumed every domain: this is what makes the canvas static afterwards.
    expect(!session.needs_frame(), "a painted static scene owes no frame");
    expect(!ui::has_any(session.root()->subtree_dirty(), ui::Dirty::Style | ui::Dirty::Layout |
                                                             ui::Dirty::Paint | ui::Dirty::Render),
           "the paint cleared the style/layout/paint/render domains");
}

void test_static_scene_presents_nothing() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "the counter starts");
    CanvasHarness canvas;
    if (!canvas.ready()) {
        expect(false, "the harness has a device and a target");
        return;
    }
    expect(canvas.draw(session), "the Run's frame is painted");
    expect(canvas.frames == 1, "one Run is one frame");

    // The property the issue's last clause names: with no events, nothing is drawn however many
    // times a caller asks. Ten attempts, zero frames - an animation loop would fail this.
    for (int attempt = 0; attempt < 10; ++attempt) {
        expect(!canvas.draw(session), "an unchanged session owes no frame");
    }
    expect(canvas.frames == 1, "a static scene presented nothing beyond its Run frame");
    expect(session.frames_painted() == 1, "the session painted exactly one frame");
}

void test_state_and_tree_survive_a_run() {
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
    (void)canvas.event(session, cli::PreviewInput::Kind::PointerDown, centre);
    const cli::CanvasPointerResult release =
        canvas.event(session, cli::PreviewInput::Kind::PointerUp, centre);
    expect(release.ran.size() == 1, "the click ran its handler");
    expect(counter_value(session) == 1, "the click incremented the counter");

    // A Run of the *same* source is a repaint, not a re-evaluation: the state the click reached
    // must still be there. This is the property the per-call snapshot driver cannot offer, and the
    // reason the session exists.
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(),
           "the second run succeeds");
    expect(session.reused(), "a run of the same source reuses the running evaluation");
    expect(counter_value(session) == 1, "the click's state survives a run of the same source");
    // A reuse owes no frame: nothing about the picture changed.
    expect(!session.needs_frame(), "a reused run owes no frame");

    // A Run of *edited* source re-evaluates and carries the state across the edit, which is the P4
    // carry rule applied to the canvas (`scene_state.hpp`).
    constexpr std::string_view kEdited = R"(export function Counter(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <Text>Total</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
)";
    expect(session.start(source_for(kEdited, "ui/Counter.tsx")).empty(),
           "the edited source starts");
    expect(!session.reused(), "an edited source re-evaluates");
    expect(counter_value(session) == 1, "the counter survives an edit to the same component");
    expect(session.needs_frame(), "an edit owes a frame");
}

void test_bad_source_reports_a_reason() {
    cli::CanvasSceneSession session;
    const std::string error =
        session.start(source_for("export function Broken(: Node {", "ui/B.tsx"));
    expect(!error.empty(), "a source that does not parse reports a reason");
    expect(!session.started(), "a failed start leaves the session unstarted");
    expect(!session.needs_frame(), "a session with no scene owes no frame");
    expect(session.root() == nullptr, "a failed start has no tree");

    CanvasHarness canvas;
    if (!canvas.ready()) {
        expect(false, "the harness has a device and a target");
        return;
    }
    expect(!canvas.draw(session), "a session with no scene paints nothing");
    expect(session.frames_painted() == 0, "a failed paint does not count as a frame");

    // The paint entry point itself reports why, rather than silently succeeding on an empty tree.
    const cli::SceneFrameResult painted = canvas.paint_anyway(session);
    expect(!painted.error.empty(), "painting an unstarted session reports why");
    expect(session.frames_painted() == 0, "a failed paint still does not count as a frame");
}

void test_pointer_report_json() {
    cli::CanvasPointerResult result;
    result.dispatched = true;
    result.changed = true;
    result.target_id = 42;
    result.hovered_id = 42;
    result.ran = {"handler::1"};
    const std::string report = cli::canvas_pointer_report_json(result);
    expect(report.find("\"pointerVersion\":1") != std::string::npos,
           "the pointer report carries its version");
    expect(report.find("\"changed\":true") != std::string::npos, "the report says a frame is owed");
    // Ids are decimal strings: a 64-bit uiId does not survive JSON.parse as a JS number.
    expect(report.find("\"hoveredId\":\"42\"") != std::string::npos,
           "ids are quoted decimal strings");
    expect(report.find("\"ran\":[\"handler::1\"]") != std::string::npos,
           "the handlers that ran are listed");
    expect(report.find("\"unrun\":[]") != std::string::npos,
           "an empty unrun list is an empty array");
    expect(report.back() == '\n', "the report is one newline-terminated line");

    // A handler id with a quote and a backslash has to survive as valid JSON: the page parses this
    // with JSON.parse, and a broken document hides the reason a click did nothing.
    cli::CanvasPointerResult odd;
    odd.ran = {"a\"b\\c"};
    const std::string escaped = cli::canvas_pointer_report_json(odd);
    expect(escaped.find("\\\"") != std::string::npos, "a quote in a handler id is escaped");
    expect(escaped.find("\\\\") != std::string::npos, "a backslash in a handler id is escaped");
}

// Issue #444's engine half: an edit that does not evaluate must not cost the running evaluation.
//
// This is the property the browser's live edit rests on. A developer typing passes through sources
// that do not parse, and each must leave the last good frame, its state cells and its painted
// picture exactly where they were; only a source that *does* evaluate may replace them.
void test_a_failed_edit_keeps_the_running_evaluation() {
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
    expect(canvas.draw(session), "the first frame is painted");
    (void)canvas.event(session, cli::PreviewInput::Kind::PointerMove, centre);
    (void)canvas.event(session, cli::PreviewInput::Kind::PointerDown, centre);
    (void)canvas.event(session, cli::PreviewInput::Kind::PointerUp, centre);
    expect(counter_value(session) == 1, "the click incremented the counter");
    expect(!session.needs_frame(), "the click's frame has been painted");
    // The baseline the next two assertions are about: whatever the click cost, it is painted.
    const std::uint32_t painted = session.frames_painted();
    expect(painted >= 2, "the click's changing frames were painted, not left owed");

    // The half-typed edit: it does not parse. The session must answer with the reason and change
    // nothing else -- no new tree, no lost state, and no frame owed (so the page draws nothing and
    // the last good picture stays on the canvas).
    const std::string error =
        session.start(source_for("export function Counter(: Node {", "ui/Counter.tsx"));
    expect(!error.empty(), "a half-typed edit reports why it did not apply");
    expect(session.started(), "a failed edit leaves the running evaluation started");
    expect(session.root() != nullptr, "a failed edit leaves the running tree in place");
    expect(!session.reused(), "a failed edit is not reported as a reuse");
    expect(counter_value(session) == 1, "a failed edit keeps the state cell it carried");
    expect(!session.needs_frame(), "a failed edit owes no frame, so nothing repaints");
    expect(!canvas.draw(session), "a failed edit paints nothing, leaving the picture up");
    expect(session.frames_painted() == painted, "the failed edit cost no painted frame");

    // The completed edit evaluates, and the state the running tree held crosses it: this is what
    // makes "type a syntax error, fix it, the count is still 1" true end to end.
    constexpr std::string_view kTyped = R"(export function Counter(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <Text>Total: {count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
)";
    expect(session.start(source_for(kTyped, "ui/Counter.tsx")).empty(),
           "the finished edit applies");
    expect(!session.reused(), "the finished edit re-evaluates");
    expect(counter_value(session) == 1, "the count survives the edit that replaced its text");
    expect(session.needs_frame(), "the finished edit owes exactly the one frame it needs");
    expect(canvas.draw(session), "the finished edit paints");
    expect(session.frames_painted() == painted + 1, "the edit cost exactly one frame");
    // The carry report the page reads: one cell crossed the edit by its own identity.
    expect(session.carry_report().carried == 1,
           "the edit carried the counter by component/cell id");
    expect(session.carry_report().dropped_component == 0,
           "nothing was dropped on a text-only edit");
}

// Issue #463: the canvas session compiles the SCOPED text of a `*.module.css`, so the class the
// lowered tree carries is the class the sheet defines. Without that, the tree says `card_0b82a5`
// and the sheet only defines `.card`: nothing matches and the component is silently unstyled.
void test_module_sheet_is_scoped() {
    constexpr std::string_view kModuleSource = R"(import styles from "./Card.module.css";
export function Card(): Node {
  return (
    <View>
      <View className={styles.card}><Text>Card</Text></View>
    </View>
  );
}
)";
    constexpr std::string_view kModuleCss = ".card { padding-top: 11px; }\n";

    // The helper first, in isolation: if this does not scope, the session cannot.
    {
        std::optional<css::ModuleScanError> scan_error;
        const std::string text =
            cli::preview_stylesheet_text(kModuleCss, "ui/Card.module.css", scan_error);
        expect(!scan_error.has_value(), "the module sheet scans");
        expect(text.find("card_0b82a5") != std::string::npos,
               "the helper scopes the class, got `" + text + "`");
    }

    cli::CanvasSceneSession session;
    cli::CanvasSessionSource source = source_for(kModuleSource, "ui/Card.tsx");
    source.stylesheet = kModuleCss;
    source.stylesheet_file = "ui/Card.module.css";
    const std::string error = session.start(source);
    expect(error.empty(), "a module sheet starts: " + error);
    if (!error.empty() || session.root() == nullptr) {
        return;
    }
    // The scoped name is the hash of the project-relative path, so it is a fixed value here.
    const ui::Node* card = find_class(*session.root(), "card_0b82a5");
    expect(card != nullptr, "the tree carries the scoped class `card_0b82a5`");
    if (card == nullptr) {
        return;
    }
    // The rule the sheet declares must reach the node: this is the assertion that fails when the
    // raw text is compiled instead of the scoped text.
    bool defines_scoped = false;
    for (const ui::style::SelectorIndex::Entry& entry : session.sheet().index().entries()) {
        for (const ui::style::CompoundSelector& compound : entry.selector.compounds) {
            for (const std::string& declared : compound.classes) {
                if (declared == "card_0b82a5") {
                    defines_scoped = true;
                }
            }
        }
    }
    expect(defines_scoped, "the session's compiled sheet defines `.card_0b82a5`");
    ui::style::SceneStyleTree tree(*session.root());
    const ui::style::ComputedStyle style = tree.compute(session.sheet(), *card);
    const std::string* padding = style.find("padding-top");
    expect(padding != nullptr && *padding == "11px",
           "the scoped module rule styles the node, got '" +
               (padding != nullptr ? *padding : std::string("<none>")) + "' (class '" +
               std::string(card->class_name()) + "')");
}

} // namespace

int main() {
    test_start_and_first_frame();
    test_static_scene_presents_nothing();
    test_state_and_tree_survive_a_run();
    test_bad_source_reports_a_reason();
    test_a_failed_edit_keeps_the_running_evaluation();
    test_pointer_report_json();
    test_module_sheet_is_scoped();
    if (failures() != 0) {
        std::cerr << "scene_canvas_session: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_session: ok\n";
    return 0;
}
