// SPDX-License-Identifier: MIT
//
// Typed text into a canvas `TextInput` (issue #601, docs/design/canvas-text-input.md): typing,
// Backspace and caret movement, IME composition, selection with copy and cut, and the
// controlled-field rule. Everything goes through the session's own `text()`/`key()`/`pointer()`,
// the calls the Wasm exports make, so a passing run is the engine editing the field, with the state
// a handler stored.

#include "scene_canvas_test_support.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "wasm_canvas_support.hpp"

namespace {

using namespace canvas_test;
namespace key = tessera::ui::key;
using Kind = cli::PreviewInput::Kind;

// A controlled field and a label that echoes the state, so the test reads the state and the paint.
constexpr std::string_view kForm = R"(export function Form(): Node {
  const [name, setName] = useState<string>("");
  return (
    <View>
      <TextInput value={name} onChange={(e) => setName(e.value)} />
      <Text>{name}</Text>
    </View>
  );
}
)";

// A controlled field whose handler never stores what was typed.
constexpr std::string_view kRefusing = R"(export function Refusing(): Node {
  const [name, setName] = useState<string>("fixed");
  return (
    <View>
      <TextInput value={name} onChange={(e) => setName("fixed")} />
    </View>
  );
}
)";

// The TextInput lives in a component the page uses as an instance: its state cell is in the
// instance's own scope, and onChange must run against it, not the root's.
constexpr std::string_view kInstance = R"(export function Page(): Node {
  return (
    <View>
      <Field />
    </View>
  );
}
export function Field(): Node {
  const [name, setName] = useState<string>("");
  return (
    <View>
      <TextInput value={name} onChange={(e) => setName(e.value)} />
      <Text>{name}</Text>
    </View>
  );
}
)";

std::string name_state(const cli::CanvasSceneSession& session) {
    for (const ui::StateCell& cell : session.state()) {
        if (cell.name != "state::name") {
            continue;
        }
        if (const std::string* value = std::get_if<std::string>(&cell.value)) {
            return *value;
        }
    }
    return "<missing>";
}

const ui::Node* field_of(const cli::CanvasSceneSession& session) {
    return find_kind(*session.root(), "TextInput");
}

// A started session with the field focused by a click, the way a person reaches it.
bool start_focused(cli::CanvasSceneSession& session, std::string_view source,
                   std::string_view file) {
    if (!session.start(source_for(source, file)).empty()) {
        expect(false, "the form source starts");
        return false;
    }
    const ui::Node* field = field_of(session);
    expect(field != nullptr, "the source has a TextInput");
    if (field == nullptr) {
        return false;
    }
    const Point at = centre_of(*field);
    (void)session.pointer(Kind::PointerMove, at.x, at.y);
    (void)session.pointer(Kind::PointerDown, at.x, at.y);
    const cli::CanvasPointerResult up = session.pointer(Kind::PointerUp, at.x, at.y);
    expect(up.field.focused, "a click on a TextInput reports it as the focused text field");
    return up.field.focused;
}

void test_typing_reaches_the_handler() {
    cli::CanvasSceneSession session;
    if (!start_focused(session, kForm, "ui/Form.tsx")) {
        return;
    }
    const cli::CanvasPointerResult a = session.text(Kind::Text, "a");
    expect(a.handled, "committed text is handled by the field");
    expect(a.changed, "typing owes a frame");
    expect(!a.ran.empty(), "the onChange body ran");
    expect(name_state(session) == "a", "the handler stored the text: " + name_state(session));
    (void)session.text(Kind::Text, "bc");
    expect(name_state(session) == "abc", "a second commit appends: " + name_state(session));

    const cli::CanvasPointerResult info = session.text(Kind::Text, "");
    expect(info.field.focused && info.field.caret == 3, "the caret is after the typed text");

    const ui::Node* field = field_of(session);
    const auto painted = field != nullptr ? field->paint_data() : std::nullopt;
    expect(painted.has_value() && painted->text == "abc", "the field paints its value");
    expect(painted.has_value() && painted->decoration.has_value() &&
               painted->decoration->caret_byte_offset == 3 && painted->decoration->caret_visible,
           "the caret is published into the paint data");

    CanvasHarness canvas;
    expect(canvas.draw(session), "the typed frame paints");
    expect(!session.needs_frame(), "and nothing more is owed");
}

void test_text_input_inside_a_component_instance() {
    cli::CanvasSceneSession session;
    if (!start_focused(session, kInstance, "ui/Instance.tsx")) {
        return;
    }
    const cli::CanvasPointerResult typed = session.text(Kind::Text, "hi");
    expect(typed.handled && !typed.ran.empty(), "the instance's onChange ran");
    const ui::Node* label = find_kind(*session.root(), "Text");
    const auto label_paint = label != nullptr ? label->paint_data() : std::nullopt;
    expect(label_paint.has_value() && label_paint->text == "hi",
           "the handler stored the text in the instance's scope and the label shows it");
    const ui::Node* field = field_of(session);
    const auto painted = field != nullptr ? field->paint_data() : std::nullopt;
    expect(painted.has_value() && painted->text == "hi",
           "the controlled field shows the instance's state after the rebuild");
    (void)session.text(Kind::Text, "!");
    const auto again = field_of(session)->paint_data();
    expect(again.has_value() && again->text == "hi!", "a second edit keeps the instance's state");
    const cli::CanvasPointerResult back = session.key(Kind::KeyDown, key::kBackspace, 0, false);
    expect(back.handled && back.field.caret == 2, "editing keys reach the instance's field");
}

void test_printable_keys_are_left_to_the_text_path() {
    cli::CanvasSceneSession session;
    if (!start_focused(session, kForm, "ui/Form.tsx")) {
        return;
    }
    const cli::CanvasPointerResult a = session.key(Kind::KeyDown, 'A', 0, false);
    expect(!a.handled, "a printable key is not handled, so the browser makes the input event");
    expect(name_state(session).empty(), "and the key alone typed nothing");
    const cli::CanvasPointerResult space = session.key(Kind::KeyUp, key::kSpace, 0, false);
    expect(!space.handled && space.ran.empty(), "Space in a field does not click it");
}

void test_backspace_and_caret_movement() {
    cli::CanvasSceneSession session;
    if (!start_focused(session, kForm, "ui/Form.tsx")) {
        return;
    }
    (void)session.text(Kind::Text, "abc");
    const cli::CanvasPointerResult back = session.key(Kind::KeyDown, key::kBackspace, 0, false);
    expect(back.handled, "Backspace is handled so the page prevents the default");
    expect(name_state(session) == "ab", "Backspace removes the last character");

    const cli::CanvasPointerResult left = session.key(Kind::KeyDown, key::kArrowLeft, 0, false);
    expect(left.handled && left.field.caret == 1, "ArrowLeft moves the caret");
    (void)session.text(Kind::Text, "X");
    expect(name_state(session) == "aXb", "text goes in at the caret: " + name_state(session));

    const cli::CanvasPointerResult home = session.key(Kind::KeyDown, key::kHome, 0, false);
    expect(home.field.caret == 0, "Home moves the caret to the start");
    (void)session.key(Kind::KeyDown, key::kDelete, 0, false);
    expect(name_state(session) == "Xb", "Delete removes the character after the caret");
}

void test_composition() {
    cli::CanvasSceneSession session;
    if (!start_focused(session, kForm, "ui/Form.tsx")) {
        return;
    }
    (void)session.text(Kind::Text, "a");
    const cli::CanvasPointerResult start = session.text(Kind::CompositionStart, "");
    expect(start.handled && start.field.composing, "a composition starts");
    const cli::CanvasPointerResult update = session.text(Kind::CompositionUpdate, "ni");
    expect(update.field.composing, "an update keeps it open");
    expect(name_state(session) == "a", "a preedit is not committed: no onChange, no state");
    const ui::Node* field = field_of(session);
    const auto preedit = field != nullptr ? field->paint_data() : std::nullopt;
    expect(preedit.has_value() && preedit->decoration.has_value() &&
               preedit->decoration->composition_active && preedit->decoration->preedit_text == "ni",
           "the preedit is published for paint");

    // Win32 and the browser commit the final string as committed text, which closes the
    // composition.
    const cli::CanvasPointerResult commit = session.text(Kind::Text, "\xE4\xBD\xA0");
    expect(commit.handled && !commit.field.composing, "the commit closes the composition");
    expect(name_state(session) == "a\xE4\xBD\xA0",
           "the committed string is the state: " + name_state(session));

    // A cancelled composition restores the field.
    (void)session.text(Kind::CompositionStart, "");
    (void)session.text(Kind::CompositionUpdate, "zz");
    const cli::CanvasPointerResult end = session.text(Kind::CompositionEnd, "");
    expect(!end.field.composing, "an end without a commit cancels");
    expect(name_state(session) == "a\xE4\xBD\xA0", "a cancelled preedit changes nothing");

    // Losing the keyboard mid-composition cancels it too.
    (void)session.text(Kind::CompositionStart, "");
    (void)session.text(Kind::CompositionUpdate, "qq");
    (void)session.blur();
    expect(name_state(session) == "a\xE4\xBD\xA0", "a blur mid-composition commits nothing");
    expect(!session.text(Kind::Text, "x").handled, "and the field no longer takes text");
}

void test_selection_copy_cut_paste() {
    cli::CanvasSceneSession session;
    if (!start_focused(session, kForm, "ui/Form.tsx")) {
        return;
    }
    (void)session.text(Kind::Text, "hello");
    const cli::CanvasPointerResult all =
        session.key(Kind::KeyDown, 'A', static_cast<std::uint32_t>(ui::Modifier::Control), false);
    expect(all.handled, "Ctrl+A is the field's select-all");
    expect(all.field.selection_start == 0 && all.field.selection_end == 5,
           "all five bytes selected");

    const cli::CanvasPointerResult copy = session.text_clipboard(false);
    expect(copy.has_text && copy.text == "hello" && copy.handled, "copy returns the selection");
    expect(name_state(session) == "hello", "copy changes nothing");

    const cli::CanvasPointerResult cut = session.text_clipboard(true);
    expect(cut.has_text && cut.text == "hello", "cut returns the selection");
    expect(name_state(session).empty(), "and deletes it: " + name_state(session));

    (void)session.text(Kind::Text, "pasted text");
    expect(name_state(session) == "pasted text", "a paste is a commit");
    const cli::CanvasPointerResult undo =
        session.key(Kind::KeyDown, 'Z', static_cast<std::uint32_t>(ui::Modifier::Control), false);
    expect(undo.handled && name_state(session).empty(),
           "Ctrl+Z undoes the paste: " + name_state(session));
}

void test_long_commit_is_not_lost() {
    cli::CanvasSceneSession session;
    if (!start_focused(session, kForm, "ui/Form.tsx")) {
        return;
    }
    const std::string long_text(700, 'q');
    (void)session.text(Kind::Text, long_text);
    expect(name_state(session) == long_text, "a paste longer than one input event arrives whole");
}

void test_controlled_field_takes_back_an_unstored_edit() {
    cli::CanvasSceneSession session;
    if (!start_focused(session, kRefusing, "ui/Refusing.tsx")) {
        return;
    }
    (void)session.text(Kind::Text, "z");
    const ui::Node* field = field_of(session);
    const auto painted = field != nullptr ? field->paint_data() : std::nullopt;
    expect(painted.has_value() && painted->text == "fixed",
           "a controlled field shows the state, not what the handler refused");
}

void test_nothing_without_a_focused_field() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kForm, "ui/Form.tsx")).empty(), "the source starts");
    const cli::CanvasPointerResult result = session.text(Kind::Text, "a");
    expect(!result.handled && !result.changed && !result.dispatched, "no focus, no text");
    expect(name_state(session).empty(), "and nothing was typed");
    expect(!session.text_clipboard(false).has_text, "and nothing to copy");
}

void test_report_and_vocabulary() {
    cli::CanvasSceneSession session;
    if (!start_focused(session, kForm, "ui/Form.tsx")) {
        return;
    }
    const std::string json = cli::canvas_pointer_report_json(session.text(Kind::Text, "ab"));
    expect(json.find("\"textFocus\":true") != std::string::npos, "the report names the text focus");
    expect(json.find("\"caret\":2") != std::string::npos, "and the caret");
    expect(json.find("\"fieldBox\":{") != std::string::npos, "and the field's box");
    expect(json.find("\"handled\":true") != std::string::npos, "and that the field took it");

    cli::CanvasSceneSession none;
    expect(none.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "a counter starts");
    expect(cli::canvas_pointer_report_json(none.text(Kind::Text, "a")).find("textFocus") ==
               std::string::npos,
           "a report with no text field carries no text fields");

    bool known = false;
    expect(cli::text_kind_from("commit", known) == Kind::Text && known, "commit");
    expect(cli::text_kind_from("start", known) == Kind::CompositionStart && known, "start");
    expect(cli::text_kind_from("update", known) == Kind::CompositionUpdate && known, "update");
    expect(cli::text_kind_from("end", known) == Kind::CompositionEnd && known, "end");
    (void)cli::text_kind_from("copy", known);
    expect(!known, "copy and cut are handled by the caller");
}

} // namespace

int main() {
    test_typing_reaches_the_handler();
    test_text_input_inside_a_component_instance();
    test_printable_keys_are_left_to_the_text_path();
    test_backspace_and_caret_movement();
    test_composition();
    test_selection_copy_cut_paste();
    test_long_commit_is_not_lost();
    test_controlled_field_takes_back_an_unstored_edit();
    test_nothing_without_a_focused_field();
    test_report_and_vocabulary();
    if (failures() != 0) {
        std::cerr << "scene_canvas_text: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_text: ok\n";
    return 0;
}
