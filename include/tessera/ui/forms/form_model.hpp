// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/task.hpp"
#include "tessera/ui/forms/form_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::forms {

// Async validator seam (docs/specs/forms-and-validation-v1.md section 2). The callback is a
// coroutine that receives the form's TaskContext (queue + cancellation token) and the immutable
// field value, and returns true when valid. The form runs it through an internal bridge coroutine
// on the configured CompletionQueue: the UI thread only starts the task, never waits for it, and
// the result arrives on the queue drain. Stale results are discarded by the field value epoch; a
// value change cancels the in-flight token, so a superseded task never mutates the field.
using AsyncValidateFn = std::function<runtime::Task<bool>(runtime::TaskContext, const FormValue&)>;

// Bounded, deterministic form model: add fields, attach sync/async rules, drive values and
// touch/blur, then gate submission. One value/validation change re-runs the synchronous rules for
// that field (and its EqualTo dependents) immediately; async rules run on blur/submit by default
// (FormConfig::validate_async_on_change opts into per-change scheduling).
//
// Threading: callers mutate the form and drain the CompletionQueue on the UI thread. Async
// validators run as tasks resumed by that queue drain; no UI-thread blocking, no detached threads.
class Form {
public:
    explicit Form(FormConfig config = {});
    ~Form();
    Form(const Form&) = delete;
    Form& operator=(const Form&) = delete;

    // Fields. Names are stable string keys; ids are assigned in declaration order starting at 1.
    // Returns nullopt when the field cap is reached or the name is empty/duplicate.
    std::optional<FieldId> add_field(std::string_view name,
                                     FormValue initial = FormValue::from_string({}),
                                     bool disabled = false, bool read_only = false);
    [[nodiscard]] std::optional<FieldId> find_field(std::string_view name) const;
    [[nodiscard]] const FieldState* field(FieldId id) const;
    [[nodiscard]] const FieldState* field(std::string_view name) const;
    [[nodiscard]] FieldState*
    mutable_field(FieldId id); // for tests/advanced hosts; keeps invariants
    [[nodiscard]] std::size_t size() const {
        return fields_.size();
    }
    [[nodiscard]] const std::vector<FieldState>& fields() const {
        return fields_;
    }
    [[nodiscard]] const FormConfig& config() const {
        return config_;
    }

    // Rules. add_rule returns false at the field/rule caps or for an invalid EqualTo target.
    bool add_rule(FieldId id, SyncRule rule);
    bool set_async_validator(FieldId id, AsyncValidateFn validator);
    bool remove_async_validator(FieldId id);
    [[nodiscard]] std::size_t rule_count(FieldId id) const;
    [[nodiscard]] bool has_async_validator(FieldId id) const;

    // Values/interaction. set_value rejects values beyond max_value_bytes (or unknown ids);
    // success marks the field dirty, re-runs sync validation and schedules async validation when
    // the field is touched and the policy allows it. touch/blur marks touched and validates.
    bool set_value(FieldId id, FormValue value);
    bool touch(FieldId id);
    bool blur(FieldId id) {
        return touch(id);
    }
    void reset_field(FieldId id);
    void reset();

    // Validation. validate_field/validate_all re-run the synchronous rules; async rules are
    // scheduled for touched fields (or forced with schedule_async=true). Return true when no
    // blocking sync issue remains (Pending is not "valid" until it settles).
    bool validate_field(FieldId id, bool schedule_async = true);
    bool validate_all(bool schedule_async = true);
    [[nodiscard]] bool valid() const;   // every enabled field is settled Valid
    [[nodiscard]] bool settled() const; // no field is Pending or Unknown
    [[nodiscard]] bool any_pending() const;
    [[nodiscard]] std::size_t pending_count() const;

    // Submit gating: can_submit() is true only when every enabled field is Valid (and touched when
    // FormConfig::require_touched_for_submit). submit() validates synchronously, returns Pending
    // when async work is in flight, Invalid with the first blocking/untouched field, or Ok. The
    // model performs no I/O: "submit" is the gate, the caller performs the request.
    [[nodiscard]] bool can_submit() const;
    [[nodiscard]] SubmitResult submit();

    // Async cancellation. bumping the epoch discards any in-flight result for that field;
    // cancel_all_async is used on teardown/reset so no callback runs after the owner is done.
    void cancel_async(FieldId id);
    void cancel_all_async();

    [[nodiscard]] const std::vector<FormIssue>& form_issues() const {
        return form_issues_;
    }
    void clear_form_issues() {
        form_issues_.clear();
    }

private:
    struct FieldMeta {
        std::vector<SyncRule> rules;
        AsyncValidateFn async_validator;
        runtime::CancellationSource cancel_source;
        std::uint64_t epoch = 0;
        bool async_in_flight = false;
        bool async_validated = false; // the current value has an applied async result
        bool async_rejected = false;  // that result was "invalid"; recompute_sync re-applies it
    };

    [[nodiscard]] bool is_enabled(const FieldState& field) const;
    void recompute_sync(std::size_t index);
    void schedule_async(std::size_t index);
    void cancel_async_at(std::size_t index);
    void apply_async_result(std::size_t index, std::uint64_t epoch, bool valid);
    [[nodiscard]] FieldId first_blocking() const;
    void cap_issue(std::string& code, std::string& message) const;

    FormConfig config_;
    std::vector<FieldState> fields_;
    std::vector<FieldMeta> meta_;
    std::vector<FormIssue> form_issues_;
};

} // namespace tessera::ui::forms
