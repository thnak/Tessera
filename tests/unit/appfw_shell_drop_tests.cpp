// SPDX-License-Identifier: MIT
//
// Drag and drop (docs/specs/platform-shell-v1.md sections 3 and 9.1; issue #450): the payload
// kinds, the budget, the path rule applied before the back end sees a file promise, and the drop
// action as a value.

#include "appfw_shell_test_support.hpp"

namespace shell_test {
namespace {

void test_file_promise_drop() {
    Harness harness;
    harness.fake.push_drop(DropAction::Copy);
    DropAction action = DropAction::None;
    bool done = false;
    auto submitted =
        harness.service.submit_drop(drop_request(file_payload(valid_path("dropped.txt"))),
                                    [&](std::expected<DropAction, ShellError> result) {
                                        done = true;
                                        expect(result.has_value(), "the drop resolved");
                                        if (result.has_value()) {
                                            action = *result;
                                        }
                                    });
    expect(submitted.has_value(), "the drop was accepted");
    expect(!done, "the drop completion does not run inline");
    pump(harness);
    expect(done, "the drop completed after a pump");
    expect(action == DropAction::Copy, "the resolved action is Copy");
    expect(harness.service.diagnostics().drops_accepted == 1, "the accepted drop is counted");
}

void test_rejected_drop_is_a_value() {
    Harness harness;
    harness.fake.push_drop(DropAction::None);
    DropAction action = DropAction::Copy;
    (void)harness.service.submit_drop(drop_request(file_payload(valid_path("x.txt"))),
                                      [&](std::expected<DropAction, ShellError> result) {
                                          expect(result.has_value(),
                                                 "a rejected drop is not an error");
                                          if (result.has_value()) {
                                              action = *result;
                                          }
                                      });
    pump(harness);
    expect(action == DropAction::None, "the rejected drop resolves to None");
    expect(harness.service.diagnostics().drops_rejected == 1, "the rejection is counted");
}

void test_a_traversal_promise_never_reaches_the_back_end() {
    Harness harness;
    auto submitted =
        harness.service.submit_drop(drop_request(file_payload("/tmp/../etc/passwd")), {});
    expect(!submitted.has_value(), "a traversal promise is refused");
    expect(submitted.error().kind == ShellErrorKind::InvalidPath, "the refusal is InvalidPath");
    expect(harness.fake.received().empty(), "a refused promise never reaches the back end");
}

void test_an_executable_promise_is_refused() {
    Harness harness;
    auto submitted =
        harness.service.submit_drop(drop_request(file_payload(valid_path("setup.exe"))), {});
    expect(!submitted.has_value() && submitted.error().kind == ShellErrorKind::InvalidPath,
           "an executable promise is refused");
    expect(harness.fake.received().empty(), "a refused promise never reaches the back end");
}

void test_text_payload_bound() {
    Harness harness;
    DragPayload payload;
    payload.kind = DragPayloadKind::Text;
    payload.text.assign(appfw::kMaxShellDragTextBytes + 1, 'x');
    auto submitted = harness.service.submit_drop(drop_request(std::move(payload)), {});
    expect(!submitted.has_value() && submitted.error().kind == ShellErrorKind::TooLarge,
           "an oversized text payload is TooLarge");
    expect(harness.fake.received().empty(), "an oversized payload never reaches the back end");
}

void test_internal_payload() {
    Harness harness;
    harness.fake.push_drop(DropAction::Move);
    DragPayload payload;
    payload.kind = DragPayloadKind::Internal;
    payload.node_id = 42;
    payload.item_key = 7;
    DropAction action = DropAction::None;
    (void)harness.service.submit_drop(drop_request(std::move(payload)),
                                      [&](std::expected<DropAction, ShellError> result) {
                                          if (result.has_value()) {
                                              action = *result;
                                          }
                                      });
    pump(harness);
    expect(action == DropAction::Move, "an internal payload resolves its action");
}

void test_drop_requires_the_capability() {
    Harness harness({}, ShellCapabilities{});
    auto submitted =
        harness.service.submit_drop(drop_request(file_payload(valid_path("a.txt"))), {});
    expect(!submitted.has_value() && submitted.error().kind == ShellErrorKind::Unsupported,
           "a drop without the capability is Unsupported");
}

void test_drag_action_helpers() {
    const DragActions both = DragActions::Copy | DragActions::Move;
    expect(appfw::has_drag_action(both, DragActions::Copy), "Copy is present");
    expect(appfw::has_drag_action(both, DragActions::Move), "Move is present");
    expect(!appfw::has_drag_action(both, DragActions::Link), "Link is absent");
    expect(!appfw::has_drag_action(DragActions::None, DragActions::Copy), "None has no action");
}

} // namespace

int run_drop_tests() {
    test_file_promise_drop();
    test_rejected_drop_is_a_value();
    test_a_traversal_promise_never_reaches_the_back_end();
    test_an_executable_promise_is_refused();
    test_text_payload_bound();
    test_internal_payload();
    test_drop_requires_the_capability();
    test_drag_action_helpers();
    return g_failures;
}

} // namespace shell_test
