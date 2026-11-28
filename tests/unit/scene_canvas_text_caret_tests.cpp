// SPDX-License-Identifier: MIT
//
// The caret of a canvas text field (issue #980): the rectangle the page opens the IME window at,
// and the pointer placing the caret and dragging a selection. The geometry is paint's, so the
// checks compare reports with one another (a caret further along the text is further right) and
// with the field's own box, never with a constant that a different font would move.

#include "scene_canvas_test_support.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "wasm_canvas_support.hpp"

namespace {

using namespace canvas_test;
namespace key = tessera::ui::key;
using Kind = cli::PreviewInput::Kind;

constexpr std::string_view kForm = R"(export function Form(): Node {
  const [name, setName] = useState<string>("");
  return (
    <View>
      <TextInput value={name} onChange={(e) => setName(e.value)} />
    </View>
  );
}
)";

const ui::Node* field_of(const cli::CanvasSceneSession& session) {
    return find_kind(*session.root(), "TextInput");
}

// A session with the field clicked into focus and `text` typed into it.
bool start_with(cli::CanvasSceneSession& session, std::string_view text) {
    if (!session.start(source_for(kForm, "ui/Form.tsx")).empty() || field_of(session) == nullptr) {
        expect(false, "the form source starts");
        return false;
    }
    const Point at = centre_of(*field_of(session));
    (void)session.pointer(Kind::PointerMove, at.x, at.y);
    (void)session.pointer(Kind::PointerDown, at.x, at.y);
    (void)session.pointer(Kind::PointerUp, at.x, at.y);
    (void)session.text(Kind::Text, text);
    return true;
}

// The caret rectangle after the caret is moved to `offset` with the keyboard.
cli::PreviewTextField caret_at(cli::CanvasSceneSession& session, std::uint32_t offset) {
    (void)session.key(Kind::KeyDown, key::kHome, 0, false);
    cli::CanvasPointerResult last = session.text(Kind::Text, "");
    for (std::uint32_t index = 0; index < offset; ++index) {
        last = session.key(Kind::KeyDown, key::kArrowRight, 0, false);
    }
    expect(last.field.caret == offset, "the caret is where the keys put it");
    return last.field;
}

float edge_of(const cli::PreviewTextField& field) {
    return field.caret_box.x;
}

void test_caret_box_follows_the_caret() {
    cli::CanvasSceneSession session;
    if (!start_with(session, "hello world")) {
        return;
    }
    const cli::PreviewTextField start = caret_at(session, 0);
    expect(start.has_caret_box, "a focused field reports its caret rectangle");
    const cli::PreviewTextField middle = caret_at(session, 5);
    const cli::PreviewTextField end = caret_at(session, 11);
    expect(edge_of(start) < edge_of(middle) && edge_of(middle) < edge_of(end),
           "the caret moves right as the offset grows");
    expect(edge_of(start) >= start.box.x && edge_of(start) < start.box.x + start.box.width / 2.0f,
           "at offset 0 the caret is at the field's left edge (inside its padding)");
    expect(start.caret_box.y >= start.box.y &&
               start.caret_box.y + start.caret_box.height <= start.box.y + start.box.height + 0.5f,
           "the caret sits inside the field vertically");
    expect(start.caret_box.height > 0.0f && start.caret_box.width >= 1.0f, "and has a size");
    expect(edge_of(end) <= end.box.x + end.box.width, "the end caret is inside the field");

    // Composing: the window belongs at the preedit's start, which is the caret before it moves.
    (void)caret_at(session, 5);
    (void)session.text(Kind::CompositionStart, "");
    const cli::CanvasPointerResult composing = session.text(Kind::CompositionUpdate, "ni");
    expect(composing.field.has_caret_box && composing.field.ime_offset == 5,
           "while composing the IME offset is where the composition started");

    const std::string json = cli::canvas_pointer_report_json(composing);
    expect(json.find("\"caretBox\":{\"x\":") != std::string::npos,
           "the report carries caretBox: " + json);
}

void test_press_places_the_caret() {
    cli::CanvasSceneSession session;
    if (!start_with(session, "hello world")) {
        return;
    }
    const cli::PreviewTextField three = caret_at(session, 3);
    const cli::PreviewTextField four = caret_at(session, 4);
    const float y = three.box.y + three.box.height / 2.0f;
    const float mid = (edge_of(three) + edge_of(four)) / 2.0f;

    (void)session.pointer(Kind::PointerMove, edge_of(three) + 0.25f, y);
    const cli::CanvasPointerResult near_three =
        session.pointer(Kind::PointerDown, edge_of(three) + 0.25f, y);
    expect(near_three.field.caret == 3, "a press just right of offset 3 puts the caret there");
    expect(near_three.field.selection_start == near_three.field.selection_end,
           "and collapses the selection");
    expect(near_three.changed, "moving the caret owes a frame");
    (void)session.pointer(Kind::PointerUp, edge_of(three) + 0.25f, y);

    const cli::CanvasPointerResult near_four = session.pointer(Kind::PointerDown, mid + 0.5f, y);
    expect(near_four.field.caret == 4, "a press past the midpoint rounds to the next boundary");
    (void)session.pointer(Kind::PointerUp, mid + 0.5f, y);

    const cli::CanvasPointerResult far_right =
        session.pointer(Kind::PointerDown, three.box.x + three.box.width - 0.5f, y);
    expect(far_right.field.caret == 11, "a press right of the text puts the caret at the end");
    (void)session.pointer(Kind::PointerUp, three.box.x + three.box.width - 0.5f, y);

    const cli::CanvasPointerResult far_left =
        session.pointer(Kind::PointerDown, three.box.x + 0.5f, y);
    expect(far_left.field.caret == 0, "a press left of the text puts the caret at the start");
    (void)session.pointer(Kind::PointerUp, three.box.x + 0.5f, y);

    // The typed text lands at the placed caret.
    (void)session.pointer(Kind::PointerDown, edge_of(three) + 0.25f, y);
    (void)session.pointer(Kind::PointerUp, edge_of(three) + 0.25f, y);
    (void)session.text(Kind::Text, "X");
    const ui::Node* field = field_of(session);
    const auto painted = field != nullptr ? field->paint_data() : std::nullopt;
    expect(painted.has_value() && painted->text == "helXlo world",
           "typing inserts at the pressed position");
}

void test_drag_selects() {
    cli::CanvasSceneSession session;
    if (!start_with(session, "hello world")) {
        return;
    }
    const cli::PreviewTextField two = caret_at(session, 2);
    const cli::PreviewTextField seven = caret_at(session, 7);
    const float y = two.box.y + two.box.height / 2.0f;
    const float from = edge_of(two) + 0.25f;
    const float to = edge_of(seven) + 0.25f;

    (void)session.pointer(Kind::PointerDown, from, y);
    const cli::CanvasPointerResult drag = session.pointer(Kind::PointerMove, to, y);
    expect(drag.field.selection_start == 2 && drag.field.selection_end == 7,
           "dragging from offset 2 to 7 selects that range");
    expect(drag.changed, "a growing selection owes a frame");
    const cli::CanvasPointerResult past =
        session.pointer(Kind::PointerMove, two.box.x + two.box.width + 40.0f, y + 30.0f);
    expect(past.field.selection_end == 11, "a drag past the field extends to the end");
    const cli::CanvasPointerResult back = session.pointer(Kind::PointerMove, from, y);
    expect(back.field.selection_start == 2 && back.field.selection_end == 2,
           "dragging back collapses the selection");
    (void)session.pointer(Kind::PointerMove, to, y);
    (void)session.pointer(Kind::PointerUp, to, y);
    const cli::CanvasPointerResult hover = session.pointer(Kind::PointerMove, from, y);
    expect(hover.field.selection_start == 2 && hover.field.selection_end == 7,
           "a move after the release does not change the selection");
    expect(session.text_clipboard(false).text == "llo w", "the selection is what copy returns");
}

void test_press_elsewhere_and_composition_leave_the_caret() {
    cli::CanvasSceneSession session;
    if (!start_with(session, "hello")) {
        return;
    }
    const cli::PreviewTextField field = caret_at(session, 1);
    (void)session.text(Kind::CompositionStart, "");
    (void)session.text(Kind::CompositionUpdate, "ni");
    const float y = field.box.y + field.box.height / 2.0f;
    const cli::CanvasPointerResult during =
        session.pointer(Kind::PointerDown, field.box.x + field.box.width - 0.5f, y);
    expect(during.field.caret == 1, "a press during a composition does not move the caret");
    (void)session.pointer(Kind::PointerUp, field.box.x + field.box.width - 0.5f, y);
    (void)session.text(Kind::CompositionEnd, "");

    // A press outside the field never starts a selection in it.
    (void)session.pointer(Kind::PointerDown, field.box.x + field.box.width + 5.0f,
                          field.box.y - 5.0f);
    const cli::CanvasPointerResult moved =
        session.pointer(Kind::PointerMove, field.box.x + 2.0f, y);
    expect(!moved.field.focused || moved.field.caret == 1,
           "a drag that began outside the field does not select inside it");
}

void test_shift_click_extends_the_selection() {
    cli::CanvasSceneSession session;
    if (!start_with(session, "hello world")) {
        return;
    }
    const cli::PreviewTextField two = caret_at(session, 2);
    const float seven = caret_at(session, 7).caret_box.x;
    (void)caret_at(session, 2);
    const float y = two.box.y + two.box.height / 2.0f;
    const cli::CanvasPointerResult shifted =
        session.pointer(Kind::PointerDown, seven + 0.25f, y, 0, true, 1);
    (void)session.pointer(Kind::PointerUp, seven + 0.25f, y, 0, true, 1);
    expect(shifted.field.selection_start == 2 && shifted.field.selection_end == 7,
           "Shift+press keeps the anchor and moves the focus to the press");
    expect(session.text_clipboard(false).text == "llo w",
           "the extended selection is what copy reads");

    // A plain press afterwards collapses it again.
    const cli::CanvasPointerResult plain =
        session.pointer(Kind::PointerDown, seven + 0.25f, y, 0, false, 1);
    (void)session.pointer(Kind::PointerUp, seven + 0.25f, y, 0, false, 1);
    expect(plain.field.selection_start == 7 && plain.field.selection_end == 7,
           "a press without Shift collapses the selection");
}

void test_double_and_triple_click_select_word_and_value() {
    cli::CanvasSceneSession session;
    if (!start_with(session, "hello, world")) {
        return;
    }
    const cli::PreviewTextField start = caret_at(session, 0);
    const float y = start.box.y + start.box.height / 2.0f;
    const float in_world = caret_at(session, 9).caret_box.x + 0.25f;
    const float in_hello = caret_at(session, 2).caret_box.x + 0.25f;
    const float on_comma = caret_at(session, 5).caret_box.x + 0.25f;

    const auto press = [&](float x, std::uint32_t clicks) {
        (void)session.pointer(Kind::PointerDown, x, y, 0, false, clicks);
        return session.pointer(Kind::PointerUp, x, y, 0, false, clicks);
    };
    cli::CanvasPointerResult word = press(in_world, 1);
    word = press(in_world, 2);
    expect(word.field.selection_start == 7 && word.field.selection_end == 12,
           "a double press selects the word under it");
    expect(session.text_clipboard(false).text == "world", "and copy reads the word");
    word = press(in_hello, 2);
    expect(word.field.selection_start == 0 && word.field.selection_end == 5, "the first word");
    word = press(on_comma, 2);
    expect(word.field.selection_start == 5 && word.field.selection_end == 6,
           "punctuation is a run of its own");
    word = press(in_world, 3);
    expect(word.field.selection_start == 0 && word.field.selection_end == 12,
           "a triple press selects the whole value");
    (void)session.text(Kind::Text, "x");
    expect(session.text(Kind::Text, "").field.caret == 1, "typing replaces the selection");
}

} // namespace

int main() {
    test_caret_box_follows_the_caret();
    test_press_places_the_caret();
    test_drag_selects();
    test_shift_click_extends_the_selection();
    test_double_and_triple_click_select_word_and_value();
    test_press_elsewhere_and_composition_leave_the_caret();
    if (failures() != 0) {
        std::cerr << "scene_canvas_text_caret: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_text_caret: ok\n";
    return 0;
}
