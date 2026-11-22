// SPDX-License-Identifier: MIT
//
// Capability negotiation (docs/specs/platform-shell-v1.md sections 1 and 9.1; gap G-15/Q-14; issue
// #450). The rule the spec states twice is that an unsupported or denied capability is a *value*,
// never a silent no-op, so every case here asserts the exact error kind rather than "it failed".

#include "appfw_shell_test_support.hpp"

namespace shell_test {
namespace {

ShellCapabilities only(ShellCapability capability) {
    ShellCapabilities capabilities;
    capabilities.supported = appfw::shell_capability_bit(capability);
    capabilities.permissioned = capabilities.supported;
    return capabilities;
}

void test_bitmask() {
    ShellCapabilities capabilities;
    capabilities.supported = appfw::shell_capability_bit(ShellCapability::ClipboardText) |
                             appfw::shell_capability_bit(ShellCapability::FileOpen);
    capabilities.permissioned = appfw::shell_capability_bit(ShellCapability::ClipboardText);
    expect(capabilities.supports(ShellCapability::ClipboardText), "a set bit is supported");
    expect(!capabilities.supports(ShellCapability::FileSave), "an unset bit is not supported");
    expect(capabilities.permits(ShellCapability::ClipboardText), "a set bit is permitted");
    expect(!capabilities.permits(ShellCapability::FileOpen), "an unset bit is not permitted");
    // The distinction the spec insists on: supported but not permitted is *not* usable, and it is a
    // different answer from unsupported.
    expect(capabilities.usable(ShellCapability::ClipboardText),
           "supported and permitted is usable");
    expect(!capabilities.usable(ShellCapability::FileOpen), "supported but denied is not usable");
    expect(!capabilities.usable(ShellCapability::FileSave), "unsupported is not usable");
}

void test_unsupported_is_a_value() {
    Harness harness({}, only(ShellCapability::FileOpen));

    auto read = harness.service.submit_clipboard_read(false, {});
    expect(!read.has_value(), "an unsupported clipboard read is refused");
    expect(read.error().kind == ShellErrorKind::Unsupported, "the refusal is Unsupported");
    expect(harness.fake.received().empty(), "an unsupported operation never reaches the back end");

    auto write = harness.service.submit_clipboard_write(ClipboardContent{"hi"}, {});
    expect(!write.has_value() && write.error().kind == ShellErrorKind::Unsupported,
           "an unsupported clipboard write is Unsupported");

    auto folder = harness.service.submit_folder_dialog(FolderDialogSpec{}, {});
    expect(!folder.has_value() && folder.error().kind == ShellErrorKind::Unsupported,
           "an unsupported folder pick is Unsupported");

    // The one capability that *is* present still works, so the refusal is per capability.
    harness.fake.push_dialog_cancel();
    auto open = harness.service.submit_open_dialog(open_spec(), {});
    expect(open.has_value(), "a supported dialog is accepted");
    pump(harness);
}

void test_denied_is_distinct_from_unsupported() {
    ShellCapabilities capabilities;
    capabilities.supported = appfw::shell_capability_bit(ShellCapability::ClipboardText);
    // Supported, but the user refused the permission.
    capabilities.permissioned = 0;
    Harness harness({}, capabilities);

    auto read = harness.service.submit_clipboard_read(false, {});
    expect(!read.has_value(), "a denied clipboard read is refused");
    expect(read.error().kind == ShellErrorKind::Denied,
           "a denied capability is Denied, not Unsupported");
    expect(harness.fake.received().empty(), "a denied operation never reaches the back end");

    const ShellDiagnostics diagnostics = harness.service.diagnostics();
    expect(diagnostics.refused_denied == 1, "the denial is counted");
    expect(diagnostics.refused_unsupported == 0, "the denial is not counted as unsupported");
}

void test_require_is_the_preflight() {
    Harness harness({}, only(ShellCapability::FileSave));
    expect(harness.service.require(ShellCapability::FileSave).has_value(),
           "a usable capability passes require");
    auto missing = harness.service.require(ShellCapability::FileOpen);
    expect(!missing.has_value() && missing.error().kind == ShellErrorKind::Unsupported,
           "a missing capability fails require with Unsupported");
}

void test_refresh_picks_up_a_permission_change() {
    ShellCapabilities capabilities;
    capabilities.supported = appfw::shell_capability_bit(ShellCapability::ClipboardText);
    capabilities.permissioned = 0;
    Harness harness({}, capabilities);

    expect(harness.service.submit_clipboard_read(false, {}).error().kind == ShellErrorKind::Denied,
           "the initial state is denied");

    // The user grants the permission; the app refreshes.
    capabilities.permissioned = capabilities.supported;
    harness.fake.set_capabilities(capabilities);
    harness.service.refresh_capabilities();

    harness.fake.push_clipboard_read(ClipboardContent{"granted"});
    auto read = harness.service.submit_clipboard_read(false, {});
    expect(read.has_value(), "after a refresh the operation is accepted");
    pump(harness);
}

void test_backend_refusal_is_unsupported() {
    Harness harness;
    harness.fake.set_available(false);
    auto read = harness.service.submit_clipboard_read(false, {});
    expect(!read.has_value() && read.error().kind == ShellErrorKind::Unsupported,
           "a back end that cannot serve the request is Unsupported");
    expect(harness.service.in_flight() == 0, "a refused operation leaves nothing in flight");
}

} // namespace

int run_capability_tests() {
    test_bitmask();
    test_unsupported_is_a_value();
    test_denied_is_distinct_from_unsupported();
    test_require_is_the_preflight();
    test_refresh_picks_up_a_permission_change();
    test_backend_refusal_is_unsupported();
    return g_failures;
}

} // namespace shell_test
