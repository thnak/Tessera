// SPDX-License-Identifier: MIT
//
// XIM pre-edit composition contract: pre-edit lifecycle, bounded payloads, malformed UTF-8 and the
// commit/cancel rules. Split out of x11_input_translation_xi2_tests.cpp to keep both files small;
// they link into the one x11_xi2_translation test executable.

#include "x11_ime_composition.hpp"
#include "x11_xi2_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace {

namespace x11 = tessera::hal::x11;
namespace ui = tessera::ui;

using x11_test::expect;
using x11_test::payload;
// --- 4. IME pre-edit -------------------------------------------------------------------------

bool ime_composition() {
    x11::CompositionState composition;
    bool ok = expect(composition.status() == x11::CompositionStatus::Idle, "a fresh state is idle");
    // Full-width "k": three UTF-8 bytes, caret after one character.
    x11::CompositionOutput output = composition.start("\357\275\213", 1);
    ok = ok &&
         expect(output.count == 1 && output.events[0].type == ui::InputEventType::CompositionStart,
                "start emits CompositionStart") &&
         expect(output.events[0].text_size == 3, "the pre-edit payload is UTF-8") &&
         expect(output.events[0].selection_start == 3 && output.events[0].selection_end == 3,
                "the caret is a UTF-8 byte offset") &&
         expect(!output.events[0].text_truncated, "a short pre-edit is not truncated") &&
         expect(composition.composing() && composition.preedit().size() == 3,
                "the composition is live");
    // Two more updates, then commit: one committed text event, then the end, and no leftover.
    output = composition.update("\357\275\213\357\275\201", 2);
    ok = ok &&
         expect(output.count == 1 && output.events[0].type == ui::InputEventType::CompositionUpdate,
                "update emits CompositionUpdate") &&
         expect(output.events[0].text_size == 6, "the update replaces the pre-edit") &&
         expect(output.events[0].selection_start == 6, "the caret follows the update");
    for (int index = 0; index < 3; ++index) {
        output = composition.update("\357\275\213\357\275\201", 2);
        ok = ok && expect(output.count == 1 &&
                              output.events[0].type == ui::InputEventType::CompositionUpdate,
                          "repeated updates stay updates");
    }
    output = composition.commit();
    ok = ok && expect(output.count == 2, "a commit emits the text and the end") &&
         expect(output.events[0].type == ui::InputEventType::EditText,
                "committed text is one EditText") &&
         expect(output.events[0].text_size == 6, "the committed payload is the pre-edit") &&
         expect(payload(output.events[0]) == "\357\275\213\357\275\201",
                "the committed bytes are the pre-edit") &&
         expect(output.events[0].selection_start == 6 && output.events[0].selection_end == 6,
                "the commit carries the caret") &&
         expect(output.events[1].type == ui::InputEventType::CompositionEnd &&
                    output.events[1].text_size == 0,
                "the end closes with an empty payload") &&
         expect(!composition.composing() && composition.preedit().empty(),
                "a commit leaves no pre-edit behind") &&
         expect(!composition.truncated(), "the commit is not truncated");
    // Commit with no composition at all is a no-op, and so is a cancel with no composition.
    output = composition.commit();
    ok = ok && expect(output.count == 0, "a commit with no composition is a no-op");
    output = composition.cancel();
    ok = ok && expect(output.count == 0, "a cancel with no composition is a no-op");
    // Cancel discards the pre-edit and never commits text.
    (void)composition.start("abc", 2);
    output = composition.cancel();
    ok = ok &&
         expect(output.count == 1 && output.events[0].type == ui::InputEventType::CompositionEnd,
                "a cancel emits only CompositionEnd") &&
         expect(composition.preedit().empty() && !composition.composing(),
                "a cancel leaves no text") &&
         expect(composition.commit().count == 0, "a commit after cancel does nothing");
    // Start -> update -> commit, where the commit's pre-edit is empty: no text, only the end.
    (void)composition.start("", 0);
    output = composition.commit();
    ok = ok &&
         expect(output.count == 1 && output.events[0].type == ui::InputEventType::CompositionEnd,
                "committing an empty pre-edit emits no text") &&
         expect(output.events[0].text_size == 0, "the empty pre-edit carries no payload");
    // A second start without a commit replaces the composition instead of appending to it.
    (void)composition.start("ab", 2);
    output = composition.start("cd", 1);
    ok =
        ok &&
        expect(output.count == 1 && output.events[0].type == ui::InputEventType::CompositionStart &&
                   payload(output.events[0]) == "cd" && output.events[0].selection_start == 1,
               "a second start replaces the composition") &&
        expect(composition.preedit() == "cd", "the replaced pre-edit is gone");
    output = composition.commit();
    ok = ok && expect(output.count == 2 && payload(output.events[0]) == "cd",
                      "the replacement is what commits");
    // A draw before the start callback still starts the composition.
    output = composition.update("xy", 0);
    ok = ok &&
         expect(output.count == 1 && output.events[0].type == ui::InputEventType::CompositionStart,
                "an update while idle starts the composition");
    (void)composition.cancel();
    return ok;
}

bool ime_clamping() {
    x11::CompositionState composition;
    // An unbounded pre-edit is cut at the documented bound on a UTF-8 boundary.
    const std::string huge(400, 'a');
    x11::CompositionOutput output = composition.start(huge, 400);
    bool ok = expect(output.count == 1, "a long pre-edit still starts") &&
              expect(output.events[0].text_size == x11::kMaxPreeditBytes,
                     "the pre-edit is clamped at the bound") &&
              expect(output.events[0].text_truncated, "the cut is reported") &&
              expect(output.events[0].selection_start == x11::kMaxPreeditBytes,
                     "the caret clamps to the kept bytes") &&
              expect(composition.truncated(), "the state reports truncation");
    // 4-byte sequences are only ever cut whole.
    std::string emoji;
    for (int index = 0; index < 100; ++index) {
        emoji += "\360\237\230\200";
    }
    output = composition.start(emoji, 100);
    ok = ok &&
         expect(output.events[0].text_size == x11::kMaxPreeditBytes,
                "a multi-byte pre-edit fills the bound exactly") &&
         expect(output.events[0].text_truncated, "the multi-byte cut is reported") &&
         expect(output.events[0].text_size % 4 == 0, "the cut is on a sequence boundary");
    // Malformed pre-edit data becomes U+FFFD, one bad byte at a time, and never overruns.
    output = composition.start("\200", 1);
    ok = ok && expect(output.events[0].text_size == 3, "a stray byte becomes U+FFFD") &&
         expect(payload(output.events[0]) == "\357\277\275", "the replacement is U+FFFD") &&
         expect(output.events[0].selection_start == 3, "the caret counts the replacement");
    output = composition.start("\303", 1);
    ok = ok &&
         expect(payload(output.events[0]) == "\357\277\275", "a truncated sequence is replaced");
    // A caret past the end clamps; a caret index inside a multi-byte character snaps forward to the
    // next character boundary (the character it points into is what the method meant).
    output = composition.start("abc", 99);
    ok = ok && expect(output.events[0].selection_start == 3, "a caret past the end clamps");
    output = composition.start("\303\251!", 1);
    ok = ok &&
         expect(output.events[0].selection_start == 2,
                "a multi-byte character advances the byte offset") &&
         expect(composition.caret_bytes() == 2, "the state keeps the byte caret") &&
         expect(x11::utf8_byte_offset("\303\251!", 2) == 3, "the offset helper counts characters");
    composition.reset();
    ok = ok && expect(!composition.composing() && composition.preedit().empty(),
                      "reset clears the composition");
    return ok;
}

} // namespace

bool ime_composition_suite() {
    return ime_composition();
}

bool ime_clamping_suite() {
    return ime_clamping();
}
