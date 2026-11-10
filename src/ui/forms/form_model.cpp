// SPDX-License-Identifier: MIT

#include "form_async.hpp"
#include "form_rules.hpp"
#include "tessera/ui/forms/form_model.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace tessera::ui::forms {
namespace {

void truncate_at_utf8_boundary(std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return;
    }
    text.resize(max_bytes);
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U) {
        text.pop_back();
    }
}

std::string bounded(std::string_view text, std::size_t max_bytes) {
    std::string result(text);
    truncate_at_utf8_boundary(result, max_bytes);
    return result;
}

FieldId next_issue_field(const FieldState& field) {
    return field.id;
}

} // namespace

Form::Form(FormConfig config)
    : config_(std::move(config)) {
    if (config_.max_fields == 0 || config_.max_value_bytes == 0 || config_.max_name_bytes == 0 ||
        config_.max_rules_per_field == 0 || config_.max_issues_per_field == 0 ||
        config_.max_form_issues == 0 || config_.max_issue_code_bytes == 0 ||
        config_.max_issue_message_bytes == 0 || config_.max_pattern_bytes == 0) {
        throw std::invalid_argument("form bounds must be greater than zero");
    }
}

Form::~Form() {
    cancel_all_async();
}

std::optional<FieldId> Form::add_field(std::string_view name, FormValue initial, bool disabled,
                                       bool read_only) {
    if (fields_.size() >= config_.max_fields || name.empty()) {
        return std::nullopt;
    }
    const bool duplicate = std::any_of(fields_.begin(), fields_.end(),
                                       [&](const FieldState& field) { return field.name == name; });
    if (duplicate) {
        return std::nullopt;
    }
    if (initial.kind == FormValueKind::String && initial.text.size() > config_.max_value_bytes) {
        return std::nullopt;
    }
    FieldState state;
    state.id = static_cast<FieldId>(fields_.size() + 1);
    state.name = bounded(name, config_.max_name_bytes);
    state.value = std::move(initial);
    state.initial = state.value;
    state.disabled = disabled;
    state.read_only = read_only;
    fields_.push_back(std::move(state));
    meta_.push_back(FieldMeta{});
    return fields_.back().id;
}

std::optional<FieldId> Form::find_field(std::string_view name) const {
    for (const FieldState& field : fields_) {
        if (field.name == name) {
            return field.id;
        }
    }
    return std::nullopt;
}

const FieldState* Form::field(FieldId id) const {
    if (id == kInvalidFieldId || id > fields_.size()) {
        return nullptr;
    }
    return &fields_[id - 1];
}

const FieldState* Form::field(std::string_view name) const {
    const std::optional<FieldId> id = find_field(name);
    return id.has_value() ? field(*id) : nullptr;
}

FieldState* Form::mutable_field(FieldId id) {
    if (id == kInvalidFieldId || id > fields_.size()) {
        return nullptr;
    }
    return &fields_[id - 1];
}

bool Form::add_rule(FieldId id, SyncRule rule) {
    if (id == kInvalidFieldId || id > fields_.size()) {
        return false;
    }
    FieldMeta& meta = meta_[id - 1];
    if (meta.rules.size() >= config_.max_rules_per_field) {
        return false;
    }
    if (rule.kind == RuleKind::EqualTo &&
        (rule.equal_to == kInvalidFieldId || rule.equal_to > fields_.size())) {
        return false;
    }
    if (rule.kind == RuleKind::Pattern && rule.pattern.size() > config_.max_pattern_bytes) {
        return false;
    }
    if (rule.kind == RuleKind::Pattern && rule.pattern.empty()) {
        return false;
    }
    if (rule.kind == RuleKind::Custom && !rule.custom) {
        return false;
    }
    if (rule.kind == RuleKind::MinLength && rule.minimum < 0) {
        return false;
    }
    if (rule.kind == RuleKind::MaxLength && rule.maximum < 0) {
        return false;
    }
    if (rule.kind == RuleKind::NumericRange && rule.minimum > rule.maximum) {
        return false;
    }
    meta.rules.push_back(std::move(rule));
    recompute_sync(static_cast<std::size_t>(id - 1));
    return true;
}

bool Form::set_async_validator(FieldId id, AsyncValidateFn validator) {
    if (id == kInvalidFieldId || id > fields_.size() || !validator) {
        return false;
    }
    meta_[id - 1].async_validator = std::move(validator);
    return true;
}

bool Form::remove_async_validator(FieldId id) {
    if (id == kInvalidFieldId || id > fields_.size()) {
        return false;
    }
    cancel_async_at(static_cast<std::size_t>(id - 1));
    meta_[id - 1].async_validator = nullptr;
    return true;
}

std::size_t Form::rule_count(FieldId id) const {
    return (id != kInvalidFieldId && id <= fields_.size()) ? meta_[id - 1].rules.size() : 0;
}

bool Form::has_async_validator(FieldId id) const {
    return id != kInvalidFieldId && id <= fields_.size() &&
           static_cast<bool>(meta_[id - 1].async_validator);
}

bool Form::set_value(FieldId id, FormValue value) {
    if (id == kInvalidFieldId || id > fields_.size()) {
        return false;
    }
    if (value.kind == FormValueKind::String && value.text.size() > config_.max_value_bytes) {
        return false;
    }
    FieldState& field_state = fields_[id - 1];
    if (field_state.disabled) {
        return false;
    }
    if (field_state.value == value) {
        return true;
    }
    FieldMeta& meta = meta_[id - 1];
    // New value: any in-flight validation is stale by definition (epoch bump + cancellation).
    cancel_async_at(static_cast<std::size_t>(id - 1));
    field_state.value = std::move(value);
    field_state.dirty = field_state.value != field_state.initial;
    recompute_sync(static_cast<std::size_t>(id - 1));
    if (field_state.touched && config_.validate_async_on_change && meta.async_validator &&
        field_state.state != ValidationState::Invalid) {
        schedule_async(static_cast<std::size_t>(id - 1));
    }
    // EqualTo dependents re-evaluate once (non-transitive, so dependency cycles cannot recurse).
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        for (const SyncRule& rule : meta_[index].rules) {
            if (rule.kind == RuleKind::EqualTo && rule.equal_to == id && index != id - 1) {
                recompute_sync(index);
                break;
            }
        }
    }
    return true;
}

bool Form::touch(FieldId id) {
    if (id == kInvalidFieldId || id > fields_.size()) {
        return false;
    }
    FieldState& field_state = fields_[id - 1];
    if (field_state.disabled) {
        return false;
    }
    const bool was_touched = field_state.touched;
    field_state.touched = true;
    meta_[id - 1].async_validated = false; // blur re-validates the current value
    meta_[id - 1].async_rejected = false;
    recompute_sync(static_cast<std::size_t>(id - 1));
    if (meta_[id - 1].async_validator && config_.completion_queue != nullptr &&
        field_state.state != ValidationState::Invalid) {
        schedule_async(static_cast<std::size_t>(id - 1));
    }
    return !was_touched || field_state.state == ValidationState::Pending;
}

void Form::reset_field(FieldId id) {
    if (id == kInvalidFieldId || id > fields_.size()) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(id - 1);
    cancel_async_at(index);
    FieldState& field_state = fields_[index];
    field_state.value = field_state.initial;
    field_state.dirty = false;
    field_state.touched = false;
    field_state.state = ValidationState::Unknown;
    field_state.issues.clear();
    field_state.issue_count = 0;
}

void Form::reset() {
    cancel_all_async();
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        FieldState& field_state = fields_[index];
        field_state.value = field_state.initial;
        field_state.dirty = false;
        field_state.touched = false;
        field_state.state = ValidationState::Unknown;
        field_state.issues.clear();
        field_state.issue_count = 0;
    }
    form_issues_.clear();
}

bool Form::is_enabled(const FieldState& field_state) const {
    return !field_state.disabled;
}

void Form::recompute_sync(std::size_t index) {
    FieldState& field_state = fields_[index];
    if (field_state.disabled) {
        field_state.issues.clear();
        field_state.issue_count = 0;
        field_state.state = ValidationState::Unknown;
        return;
    }
    const auto push_issue = [&](std::string code, std::string message, IssueSeverity severity) {
        ++field_state.issue_count;
        if (field_state.issues.size() >= config_.max_issues_per_field) {
            return; // counted, not stored
        }
        FormIssue issue;
        issue.code = bounded(code, config_.max_issue_code_bytes);
        issue.message = bounded(message, config_.max_issue_message_bytes);
        issue.field = next_issue_field(field_state);
        issue.severity = severity;
        field_state.issues.push_back(std::move(issue));
    };

    field_state.issues.clear();
    field_state.issue_count = 0;
    for (const SyncRule& rule : meta_[index].rules) {
        const FormValue* equal_to_value = nullptr;
        if (rule.kind == RuleKind::EqualTo && rule.equal_to != kInvalidFieldId &&
            rule.equal_to <= fields_.size()) {
            equal_to_value = &fields_[rule.equal_to - 1].value;
        }
        const std::optional<RuleIssueTemplate> rule_issue =
            detail::evaluate_sync_rule(rule, field_state.value, equal_to_value);
        if (rule_issue.has_value()) {
            push_issue(rule_issue->code, rule_issue->message, IssueSeverity::Error);
        }
    }
    // An applied async rejection stays in force until the value changes or the field is
    // re-validated; recompute_sync clears issues, so it must put the rejection back.
    if (meta_[index].async_validated && meta_[index].async_rejected) {
        push_issue("async_invalid", "Invalid", IssueSeverity::Error);
    }
    if (field_state.issue_count > 0) {
        field_state.state = ValidationState::Invalid;
        return;
    }
    if (meta_[index].async_in_flight) {
        field_state.state = ValidationState::Pending;
        return;
    }
    field_state.state = ValidationState::Valid;
}

void Form::schedule_async(std::size_t index) {
    FieldMeta& meta = meta_[index];
    FieldState& field_state = fields_[index];
    if (!meta.async_validator || config_.completion_queue == nullptr) {
        return;
    }
    cancel_async_at(index);
    const std::uint64_t epoch = ++meta.epoch;
    meta.cancel_source = runtime::CancellationSource{};
    meta.async_in_flight = true;
    meta.async_validated = false;
    meta.async_rejected = false;
    field_state.state = ValidationState::Pending;
    runtime::TaskContext context;
    context.queue = config_.completion_queue;
    context.priority = runtime::CompletionPriority::Visible;
    context.token = meta.cancel_source.token();
    runtime::Task<void> task = detail::run_async_bridge(
        context, meta.async_validator, field_state.value,
        [this, index, epoch](bool valid) { apply_async_result(index, epoch, valid); });
    if (!task.start()) {
        // Queue full/missing: fall back to the synchronous outcome and report no async result.
        meta.async_in_flight = false;
        field_state.state =
            field_state.issue_count > 0 ? ValidationState::Invalid : ValidationState::Valid;
    }
}

void Form::cancel_async_at(std::size_t index) {
    FieldMeta& meta = meta_[index];
    ++meta.epoch;
    meta.cancel_source.cancel();
    meta.async_in_flight = false;
    meta.async_validated = false;
    meta.async_rejected = false;
    FieldState& field_state = fields_[index];
    if (field_state.state == ValidationState::Pending) {
        field_state.state =
            field_state.issue_count > 0 ? ValidationState::Invalid : ValidationState::Valid;
    }
}

void Form::apply_async_result(std::size_t index, std::uint64_t epoch, bool valid) {
    if (index >= fields_.size()) {
        return;
    }
    FieldMeta& meta = meta_[index];
    if (!meta.async_in_flight || epoch != meta.epoch) {
        return; // stale: value changed / cancelled after this validation started
    }
    meta.async_in_flight = false;
    meta.async_validated = true;
    meta.async_rejected = !valid;
    FieldState& field_state = fields_[index];
    if (valid) {
        field_state.state =
            field_state.issue_count > 0 ? ValidationState::Invalid : ValidationState::Valid;
        return;
    }
    ++field_state.issue_count;
    if (field_state.issues.size() < config_.max_issues_per_field) {
        FormIssue issue;
        issue.code = bounded("async_invalid", config_.max_issue_code_bytes);
        issue.message = bounded("Invalid", config_.max_issue_message_bytes);
        issue.field = field_state.id;
        issue.severity = IssueSeverity::Error;
        field_state.issues.push_back(std::move(issue));
    }
    field_state.state = ValidationState::Invalid;
}

void Form::cancel_async(FieldId id) {
    if (id == kInvalidFieldId || id > fields_.size()) {
        return;
    }
    cancel_async_at(static_cast<std::size_t>(id - 1));
}

void Form::cancel_all_async() {
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        cancel_async_at(index);
    }
}

bool Form::validate_field(FieldId id, bool schedule_async_validation) {
    if (id == kInvalidFieldId || id > fields_.size()) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(id - 1);
    recompute_sync(index);
    if (schedule_async_validation && meta_[index].async_validator &&
        config_.completion_queue != nullptr && !meta_[index].async_validated &&
        fields_[index].state != ValidationState::Invalid) {
        schedule_async(index);
    }
    return fields_[index].state == ValidationState::Valid;
}

bool Form::validate_all(bool schedule_async_validation) {
    bool all_valid = true;
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        recompute_sync(index);
        if (schedule_async_validation && meta_[index].async_validator &&
            config_.completion_queue != nullptr && !meta_[index].async_validated &&
            fields_[index].state != ValidationState::Invalid) {
            schedule_async(index);
        }
        if (is_enabled(fields_[index]) && fields_[index].state != ValidationState::Valid) {
            all_valid = false;
        }
    }
    return all_valid;
}

bool Form::valid() const {
    for (const FieldState& field_state : fields_) {
        if (is_enabled(field_state) && field_state.state != ValidationState::Valid) {
            return false;
        }
    }
    return true;
}

bool Form::settled() const {
    for (const FieldState& field_state : fields_) {
        if (is_enabled(field_state) && (field_state.state == ValidationState::Pending ||
                                        field_state.state == ValidationState::Unknown)) {
            return false;
        }
    }
    return true;
}

bool Form::any_pending() const {
    return pending_count() > 0;
}

std::size_t Form::pending_count() const {
    std::size_t count = 0;
    for (const FieldState& field_state : fields_) {
        if (field_state.state == ValidationState::Pending) {
            ++count;
        }
    }
    return count;
}

FieldId Form::first_blocking() const {
    for (const FieldState& field_state : fields_) {
        if (!is_enabled(field_state)) {
            continue;
        }
        const bool needs_touch = config_.require_touched_for_submit && !field_state.touched;
        if (needs_touch || field_state.state != ValidationState::Valid) {
            return field_state.id;
        }
    }
    return kInvalidFieldId;
}

bool Form::can_submit() const {
    return first_blocking() == kInvalidFieldId && !any_pending();
}

SubmitResult Form::submit() {
    SubmitResult result;
    validate_all(true);
    result.pending = pending_count();
    if (result.pending > 0) {
        result.status = SubmitStatus::Pending;
        result.first_invalid = first_blocking();
        return result;
    }
    result.first_invalid = first_blocking();
    if (result.first_invalid != kInvalidFieldId) {
        result.status = SubmitStatus::Invalid;
        return result;
    }
    result.status = SubmitStatus::Ok;
    return result;
}

} // namespace tessera::ui::forms
