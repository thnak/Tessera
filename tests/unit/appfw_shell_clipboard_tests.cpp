// SPDX-License-Identifier: MIT
//
// Clipboard (docs/specs/platform-shell-v1.md section 2; issue #450): the round trip, the size
// bound, the secure-field rule, and "never blocks the UI thread" - which here means the completion
// is posted rather than run inline.

#include "appfw_shell_test_support.hpp"

namespace shell_test {
namespace {

void test_round_trip() {
    Harness harness;
    harness.fake.push_clipboard_read(ClipboardContent{"hello"});
    harness.fake.push_clipboard_write(true);

    std::optional<ClipboardContent> read_result;
    bool read_done = false;
    auto read = harness.service.submit_clipboard_read(
        false, [&](std::expected<std::optional<ClipboardContent>, ShellError> result) {
            read_done = true;
            expect(result.has_value(), "the read succeeded");
            if (result.has_value()) {
                read_result = std::move(*result);
            }
        });
    expect(read.has_value(), "the read was accepted");
    // The completion must not have run inline: the back end posts it.
    expect(!read_done, "the read completion does not run on the submitting stack");
    pump(harness);
    expect(read_done, "the read completed after a pump");
    expect(read_result.has_value() && read_result->text == "hello",
           "the clipboard text round-trips");

    bool write_done = false;
    auto write = harness.service.submit_clipboard_write(
        ClipboardContent{"world"}, [&](std::expected<void, ShellError> result) {
            write_done = true;
            expect(result.has_value(), "the write succeeded");
        });
    expect(write.has_value(), "the write was accepted");
    expect(!write_done, "the write completion does not run inline");
    pump(harness);
    expect(write_done, "the write completed after a pump");

    const auto& received = harness.fake.received();
    expect(received.size() == 2, "the back end saw both operations");
    expect(received[1].text == "world", "the back end saw the written text");
}

void test_empty_clipboard_is_a_clean_read() {
    Harness harness;
    // The script's default is an empty clipboard, which is `std::nullopt` and not a failure.
    std::optional<ClipboardContent> result;
    bool done = false;
    (void)harness.service.submit_clipboard_read(
        false, [&](std::expected<std::optional<ClipboardContent>, ShellError> value) {
            done = true;
            expect(value.has_value(), "an empty clipboard is not an error");
            if (value.has_value()) {
                result = std::move(*value);
            }
        });
    pump(harness);
    expect(done, "the read completed");
    expect(!result.has_value(), "an empty clipboard is nullopt");
}

void test_size_bound() {
    Harness harness;
    // A write over the bound is refused before the back end sees it.
    ClipboardContent oversized;
    oversized.text.assign(appfw::kMaxShellClipboardBytes + 1, 'x');
    auto write = harness.service.submit_clipboard_write(std::move(oversized), {});
    expect(!write.has_value() && write.error().kind == ShellErrorKind::TooLarge,
           "an oversized write is TooLarge");
    expect(harness.fake.received().empty(), "an oversized write never reaches the back end");

    // A read that comes back over the bound is refused rather than truncated.
    ClipboardContent big;
    big.text.assign(appfw::kMaxShellClipboardBytes + 1, 'y');
    harness.fake.push_clipboard_read(std::move(big));
    bool done = false;
    (void)harness.service.submit_clipboard_read(
        false, [&](std::expected<std::optional<ClipboardContent>, ShellError> result) {
            done = true;
            expect(!result.has_value(), "an oversized read is refused");
            if (!result.has_value()) {
                expect(result.error().kind == ShellErrorKind::TooLarge,
                       "an oversized read is TooLarge");
            }
        });
    pump(harness);
    expect(done, "the oversized read completed as a value");
}

void test_secure_field() {
    Harness harness;
    auto refused = harness.service.submit_clipboard_read(true, {});
    expect(!refused.has_value() && refused.error().kind == ShellErrorKind::Denied,
           "a secure field's read is denied by default");
    expect(harness.fake.received().empty(), "a secure read never reaches the back end");

    // The explicit opt-in is the only way through, and it is configuration, not a per-call flag.
    ShellConfig config;
    config.allow_secure_clipboard = true;
    Harness allowed(config);
    allowed.fake.push_clipboard_read(ClipboardContent{"secret"});
    auto read = allowed.service.submit_clipboard_read(true, {});
    expect(read.has_value(), "the opt-in allows a secure read");
    pump(allowed);
}

void test_one_at_a_time() {
    Harness harness;
    harness.fake.push_clipboard_read(ClipboardContent{"first"}, 100);
    auto first = harness.service.submit_clipboard_read(false, {});
    expect(first.has_value(), "the first read is accepted");
    auto second = harness.service.submit_clipboard_read(false, {});
    expect(!second.has_value() && second.error().kind == ShellErrorKind::Busy,
           "a second read while one is in flight is Busy");
    expect(harness.service.diagnostics().refused_busy == 1, "the refusal is counted");

    advance(harness, 0.2);
    // Once the first resolved, a new one is accepted again.
    harness.fake.push_clipboard_read(ClipboardContent{"second"});
    auto third = harness.service.submit_clipboard_read(false, {});
    expect(third.has_value(), "after the first resolved a new read is accepted");
    pump(harness);
}

void test_cancel() {
    Harness harness;
    harness.fake.push_clipboard_read(ClipboardContent{"late"}, 100);
    auto read = harness.service.submit_clipboard_read(false, {});
    expect(read.has_value(), "the read is accepted");
    expect(harness.service.cancel_clipboard(), "the in-flight read can be cancelled");
    expect(!harness.service.cancel_clipboard(), "a second cancel is a no-op");
    // The fake observes the token and drops the operation, so nothing is delivered.
    advance(harness, 0.2);
    expect(harness.fake.cancelled_operations() == 1, "the back end observed the cancellation");
}

void test_diagnostics_are_bounded() {
    ShellConfig config;
    config.diagnostic_capacity = 2;
    Harness harness(config, ShellCapabilities{});
    for (int index = 0; index < 5; ++index) {
        (void)harness.service.submit_clipboard_read(false, {});
    }
    const ShellDiagnostics diagnostics = harness.service.diagnostics();
    expect(diagnostics.entries.size() == 2, "the ring keeps only its capacity");
    expect(diagnostics.dropped == 3, "the evictions are counted");
    expect(diagnostics.recorded == 5, "every failure is counted even when evicted");
    expect(diagnostics.refused_unsupported == 5, "the counter is not bounded by the ring");
    // Section 8: a diagnostic never carries clipboard content.
    for (const auto& entry : diagnostics.entries) {
        expect(entry.detail.find("secret") == std::string::npos,
               "a diagnostic never carries clipboard content");
    }
}

} // namespace

int run_clipboard_tests() {
    test_round_trip();
    test_empty_clipboard_is_a_clean_read();
    test_size_bound();
    test_secure_field();
    test_one_at_a_time();
    test_cancel();
    test_diagnostics_are_bounded();
    return g_failures;
}

} // namespace shell_test
