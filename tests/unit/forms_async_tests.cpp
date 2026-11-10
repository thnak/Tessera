// SPDX-License-Identifier: MIT
//
// Forms async validation (task-78): pending visibility, queue-driven completion, stale-result
// discard by epoch, cancellation, on-change policy, submit gating while pending, destructor safety
// and determinism.

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/task.hpp"
#include "tessera/ui/forms/form_model.hpp"

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using tessera::runtime::CompletionQueue;
using tessera::runtime::Task;
using tessera::runtime::TaskContext;
using tessera::ui::forms::Form;
using tessera::ui::forms::FormConfig;
using tessera::ui::forms::FormValue;
using tessera::ui::forms::RuleIssueTemplate;
using tessera::ui::forms::RuleKind;
using tessera::ui::forms::SubmitStatus;
using tessera::ui::forms::SyncRule;
using tessera::ui::forms::ValidationState;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "forms_async: " << message << "\n";
    }
    return condition;
}

Task<bool> not_taken(TaskContext, const FormValue& value) {
    co_return value.text != "taken";
}

Task<bool> always_invalid(TaskContext, const FormValue&) {
    co_return false;
}

FormConfig config_with_queue(CompletionQueue& queue) {
    FormConfig config;
    config.completion_queue = &queue;
    return config;
}

bool pending_then_valid() {
    CompletionQueue queue(8);
    Form form(config_with_queue(queue));
    const auto name = form.add_field("name", FormValue::from_string(""));
    SyncRule required;
    required.kind = RuleKind::Required;
    form.add_rule(*name, required);
    if (!expect(form.set_async_validator(*name, &not_taken), "async validator attached")) {
        return false;
    }
    form.set_value(*name, FormValue::from_string("Ada"));
    form.touch(*name);
    if (!expect(form.field(*name)->state == ValidationState::Pending,
                "touch schedules async validation and shows Pending") ||
        !expect(form.any_pending() && form.pending_count() == 1, "pending is visible") ||
        !expect(!form.valid() && !form.settled(), "pending is not valid/settled")) {
        return false;
    }
    if (!expect(queue.size() > 0, "the bridge task is queued")) {
        return false;
    }
    queue.run(8);
    if (!expect(form.field(*name)->state == ValidationState::Valid, "queue drain settles Valid") ||
        !expect(form.valid() && form.settled() && !form.any_pending(), "settled state") ||
        !expect(form.submit().status == SubmitStatus::Ok, "valid form submits")) {
        return false;
    }
    return true;
}

bool invalid_async_state() {
    CompletionQueue queue(8);
    Form form(config_with_queue(queue));
    const auto name = form.add_field("name", FormValue::from_string("taken"));
    form.set_async_validator(*name, &not_taken);
    form.touch(*name);
    queue.run(8);
    if (!expect(form.field(*name)->state == ValidationState::Invalid,
                "async rejection settles Invalid") ||
        !expect(!form.field(*name)->issues.empty() &&
                    form.field(*name)->issues.back().code == "async_invalid",
                "async rejection adds the stable async_invalid issue") ||
        !expect(!form.can_submit(), "async invalid blocks submit")) {
        return false;
    }
    return true;
}

bool rejection_survives_revalidation() {
    CompletionQueue queue(8);
    Form form(config_with_queue(queue));
    const auto name = form.add_field("name", FormValue::from_string("taken"));
    form.set_async_validator(*name, &not_taken);
    form.touch(*name);
    queue.run(8);
    if (!expect(form.field(*name)->state == ValidationState::Invalid, "rejected before submit")) {
        return false;
    }
    const auto result = form.submit();
    queue.run(8);
    return expect(result.status != tessera::ui::forms::SubmitStatus::Ok,
                  "submit does not accept a rejected value") &&
           expect(form.field(*name)->state == ValidationState::Invalid,
                  "re-validation keeps the async rejection") &&
           expect(!form.validate_all(false), "validate_all keeps the async rejection") &&
           expect(!form.can_submit(), "rejected field still blocks submit");
}

bool stale_result_discarded() {
    CompletionQueue queue(8);
    Form form(config_with_queue(queue));
    const auto name = form.add_field("name", FormValue::from_string(""));
    form.set_async_validator(*name, &not_taken);
    form.touch(*name);
    if (!expect(form.field(*name)->state == ValidationState::Pending, "first validation pending")) {
        return false;
    }
    // The value changes before the queue drains: the in-flight result is stale by epoch.
    form.set_value(*name, FormValue::from_string("taken"));
    if (!expect(form.field(*name)->state != ValidationState::Pending,
                "a value change cancels the in-flight validation")) {
        return false;
    }
    queue.run(8);
    if (!expect(form.field(*name)->state == ValidationState::Valid,
                "the stale 'not taken' result is discarded (sync state kept)") ||
        !expect(form.field(*name)->issue_count == 0, "no stale issue is recorded")) {
        return false;
    }
    // Touching again validates the current value: async rejection applies.
    form.touch(*name);
    if (!expect(form.field(*name)->state == ValidationState::Pending, "revalidated pending")) {
        return false;
    }
    queue.run(8);
    return expect(form.field(*name)->state == ValidationState::Invalid,
                  "the fresh result applies to the new value");
}

bool cancellation() {
    CompletionQueue queue(8);
    Form form(config_with_queue(queue));
    const auto name = form.add_field("name", FormValue::from_string(""));
    form.set_async_validator(*name, &always_invalid);
    form.touch(*name);
    if (!expect(form.field(*name)->state == ValidationState::Pending, "pending before cancel")) {
        return false;
    }
    form.cancel_async(*name);
    if (!expect(form.field(*name)->state != ValidationState::Pending, "cancel clears pending") ||
        !expect(form.field(*name)->state == ValidationState::Valid,
                "cancel keeps the synchronous result")) {
        return false;
    }
    const std::size_t before = queue.size();
    queue.run(8);
    (void)before;
    return expect(form.field(*name)->state == ValidationState::Valid,
                  "a cancelled result never arrives");
}

bool on_change_policy() {
    CompletionQueue queue(8);
    FormConfig config = config_with_queue(queue);
    config.validate_async_on_change = true;
    Form form(config);
    const auto name = form.add_field("name", FormValue::from_string("ok"));
    form.set_async_validator(*name, &not_taken);
    form.touch(*name);
    queue.run(8);
    if (!expect(form.field(*name)->state == ValidationState::Valid, "initial validation valid")) {
        return false;
    }
    form.set_value(*name, FormValue::from_string("taken"));
    if (!expect(form.field(*name)->state == ValidationState::Pending,
                "on-change policy schedules async on each value change")) {
        return false;
    }
    queue.run(8);
    return expect(form.field(*name)->state == ValidationState::Invalid, "on-change result applies");
}

bool submit_while_pending_then_ok() {
    CompletionQueue queue(8);
    Form form(config_with_queue(queue));
    const auto name = form.add_field("name", FormValue::from_string(""));
    form.set_async_validator(*name, &not_taken);
    form.touch(*name);
    const auto pending = form.submit();
    if (!expect(pending.status == SubmitStatus::Pending, "submit reports Pending") ||
        !expect(pending.pending == 1, "submit reports the pending count") ||
        !expect(pending.first_invalid == *name, "pending submit names the field")) {
        return false;
    }
    queue.run(8);
    return expect(form.submit().status == SubmitStatus::Ok, "submit succeeds after settling");
}

bool no_queue_means_no_async() {
    Form form;
    const auto name = form.add_field("name", FormValue::from_string("x"));
    form.set_async_validator(*name, &always_invalid);
    form.touch(*name);
    return expect(form.field(*name)->state == ValidationState::Valid,
                  "without a completion queue async rules are ignored (sync result)");
}

bool destructor_safety() {
    CompletionQueue queue(8);
    {
        Form form(config_with_queue(queue));
        const auto name = form.add_field("name", FormValue::from_string(""));
        form.set_async_validator(*name, &always_invalid);
        form.touch(*name);
        if (!expect(form.field(*name)->state == ValidationState::Pending, "pending at teardown")) {
            return false;
        }
    }
    // The form is gone; the queued bridge resume must be dropped, not run against a dead form.
    queue.run(8);
    return expect(queue.empty(), "teardown cancels the queued completion");
}

bool determinism() {
    const auto run = [] {
        CompletionQueue queue(8);
        Form form(config_with_queue(queue));
        const auto name = form.add_field("name", FormValue::from_string(""));
        SyncRule required;
        required.kind = RuleKind::Required;
        form.add_rule(*name, required);
        form.set_async_validator(*name, &not_taken);
        std::vector<ValidationState> states;
        form.set_value(*name, FormValue::from_string("Ada"));
        form.touch(*name);
        states.push_back(form.field(*name)->state);
        queue.run(8);
        states.push_back(form.field(*name)->state);
        form.set_value(*name, FormValue::from_string("taken"));
        states.push_back(form.field(*name)->state);
        form.touch(*name);
        states.push_back(form.field(*name)->state);
        queue.run(8);
        states.push_back(form.field(*name)->state);
        return std::pair{form.fields(), states};
    };
    return expect(run() == run(), "the same async script is deterministic");
}

} // namespace

int main() {
    if (!pending_then_valid()) {
        return 1;
    }
    if (!invalid_async_state()) {
        return 2;
    }
    if (!stale_result_discarded()) {
        return 3;
    }
    if (!cancellation()) {
        return 4;
    }
    if (!on_change_policy()) {
        return 5;
    }
    if (!submit_while_pending_then_ok()) {
        return 6;
    }
    if (!no_queue_means_no_async()) {
        return 7;
    }
    if (!destructor_safety()) {
        return 8;
    }
    if (!determinism()) {
        return 9;
    }
    if (!rejection_survives_revalidation()) {
        return 10;
    }

    std::cout << "forms_async: ok\n";
    return 0;
}
