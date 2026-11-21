// SPDX-License-Identifier: MIT
//
// `clipboard` (ADR-064 item 7, issue #632): copy text with feedback state.
//
// The engine owns the mechanism (`hal::Clipboard`, with `MemoryClipboard` as the deterministic test
// implementation); this behavior owns the hook's half: the write, the `copied`/`failed` outputs,
// and the bounded feedback window that clears itself. The window is a deadline read from the
// services' clock, so a test advances it explicitly rather than sleeping.

#include "tessera/hal/clipboard.hpp"
#include "tessera/ui/behavior/clipboard_copy.hpp"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::hal::ClipboardError;
using tessera::hal::MemoryClipboard;
using tessera::hal::UnsupportedClipboard;
using tessera::ui::behavior::ArgsView;
using tessera::ui::behavior::ArgValue;
using tessera::ui::behavior::ClipboardCopy;
using tessera::ui::behavior::OutputValue;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "clipboard_copy: " << message << "\n";
        ++failures;
    }
}

ArgValue string_arg(std::string_view value) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::String;
    arg.text = value;
    return arg;
}

std::array<ArgValue, 1> options(std::string_view text) {
    return {string_arg(text)};
}

void test_copy_writes_and_reports() {
    MemoryClipboard clipboard;
    ClipboardCopy behavior;
    auto args = options("hello");
    behavior.attach(Targets{}, Services{nullptr, &clipboard, 0.0}, ArgsView{args});
    expect(clipboard.text() == "hello", "the text reached the clipboard");
    const auto outputs = behavior.outputs();
    expect(outputs.size() == 2, "two outputs");
    expect(outputs[0].kind == OutputValue::Kind::Bool && outputs[0].boolean,
           "`copied` reports success");
    expect(!outputs[1].boolean, "`failed` is false on success");
}

void test_feedback_clears_after_the_window() {
    MemoryClipboard clipboard;
    ClipboardCopy behavior;
    auto args = options("hello");
    behavior.attach(Targets{}, Services{nullptr, &clipboard, 0.0}, ArgsView{args});
    expect(behavior.outputs()[0].boolean, "the feedback is up right after the copy");

    behavior.tick(ClipboardCopy::kFeedbackSeconds - 0.1);
    expect(behavior.outputs()[0].boolean, "the feedback is still up inside the window");

    behavior.tick(ClipboardCopy::kFeedbackSeconds + 0.1);
    expect(!behavior.outputs()[0].boolean, "the feedback clears after the window");
    expect(!behavior.outputs()[1].boolean, "and `failed` stays false");
}

void test_unsupported_clipboard_reports_failure() {
    UnsupportedClipboard clipboard;
    ClipboardCopy behavior;
    auto args = options("hello");
    behavior.attach(Targets{}, Services{nullptr, &clipboard, 0.0}, ArgsView{args});
    const auto outputs = behavior.outputs();
    expect(!outputs[0].boolean, "`copied` is false when the platform refuses");
    expect(outputs[1].boolean, "`failed` is true when the platform refuses");
}

void test_no_clipboard_reports_failure() {
    ClipboardCopy behavior;
    auto args = options("hello");
    behavior.attach(Targets{}, Services{}, ArgsView{args});
    expect(!behavior.outputs()[0].boolean, "`copied` is false with no seam");
    expect(behavior.outputs()[1].boolean, "`failed` is true with no seam");
}

void test_empty_text_copies_nothing() {
    MemoryClipboard clipboard;
    const auto seeded = clipboard.write_text("previous");
    expect(seeded.has_value(), "the fixture seeds the clipboard");
    ClipboardCopy behavior;
    auto args = options("");
    behavior.attach(Targets{}, Services{nullptr, &clipboard, 0.0}, ArgsView{args});
    expect(clipboard.text() == "previous", "an empty text does not overwrite the user's clipboard");
    expect(!behavior.outputs()[0].boolean, "and reports no copy");
}

void test_detach_resets() {
    MemoryClipboard clipboard;
    ClipboardCopy behavior;
    auto args = options("hello");
    behavior.attach(Targets{}, Services{nullptr, &clipboard, 0.0}, ArgsView{args});
    behavior.detach();
    expect(!behavior.outputs()[0].boolean, "a detached behavior reports the manifest default");
    expect(!behavior.outputs()[1].boolean, "both outputs are false");
    expect(behavior.clipboard() == nullptr, "a detached behavior holds no seam");
}

// A behavior destroyed without an explicit detach must not keep the seam.
void test_destructor_resets() {
    MemoryClipboard clipboard;
    {
        ClipboardCopy behavior;
        auto args = options("hello");
        behavior.attach(Targets{}, Services{nullptr, &clipboard, 0.0}, ArgsView{args});
        expect(behavior.clipboard() == &clipboard, "the seam is held inside the scope");
    }
    expect(clipboard.text() == "hello", "the copy already happened");
}

// A copy that fails still starts the feedback window, so a "Copy failed" label clears itself too.
void test_failure_feedback_also_expires() {
    UnsupportedClipboard clipboard;
    ClipboardCopy behavior;
    auto args = options("hello");
    behavior.attach(Targets{}, Services{nullptr, &clipboard, 0.0}, ArgsView{args});
    expect(behavior.outputs()[1].boolean, "the failure is reported");
    behavior.tick(ClipboardCopy::kFeedbackSeconds + 0.1);
    expect(!behavior.outputs()[1].boolean, "the failure feedback clears after the window");
}

} // namespace

int main() {
    test_copy_writes_and_reports();
    test_feedback_clears_after_the_window();
    test_unsupported_clipboard_reports_failure();
    test_no_clipboard_reports_failure();
    test_empty_text_copies_nothing();
    test_detach_resets();
    test_destructor_resets();
    test_failure_feedback_also_expires();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
