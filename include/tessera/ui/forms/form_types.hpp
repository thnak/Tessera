// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::runtime {
class CompletionQueue;
}

namespace tessera::ui::forms {

// Forms and validation core (docs/specs/forms-and-validation-v1.md). Bounded, deterministic state
// model: forms are data + rules, not widgets. UI code drives values/interaction; validation results
// are values (no exceptions), and asynchronous rules run through the runtime's CompletionQueue so
// the UI thread never blocks.
using FieldId = std::uint32_t;

// Stable per-form id, assigned in declaration order starting at 1. 0 means "no field" and is also
// used for form-level issues.
inline constexpr FieldId kInvalidFieldId = 0;

enum class FormValueKind : std::uint8_t { String, Enum, Bool };

// A small typed value: string (bounded by FormConfig::max_value_bytes), integer enum (>= 0) or
// bool. Initial values are copied once; updates copy the caller string.
struct FormValue {
    FormValueKind kind = FormValueKind::String;
    std::string text;
    std::int32_t enum_value = 0;
    bool bool_value = false;

    [[nodiscard]] static FormValue from_string(std::string_view value) {
        FormValue result;
        result.kind = FormValueKind::String;
        result.text.assign(value.data(), value.size());
        return result;
    }
    [[nodiscard]] static FormValue from_enum(std::int32_t value) {
        FormValue result;
        result.kind = FormValueKind::Enum;
        result.enum_value = value;
        return result;
    }
    [[nodiscard]] static FormValue from_bool(bool value) {
        FormValue result;
        result.kind = FormValueKind::Bool;
        result.bool_value = value;
        return result;
    }

    [[nodiscard]] bool empty_string() const {
        return kind == FormValueKind::String && text.empty();
    }

    bool operator==(const FormValue&) const = default;
};

enum class ValidationState : std::uint8_t {
    Unknown, // no validation has run since the value/touch change
    Valid,
    Invalid,
    Pending, // an async rule is in flight
};

enum class IssueSeverity : std::uint8_t { Error, Warning };

struct FormIssue {
    std::string code;    // stable machine code, e.g. "required"
    std::string message; // short fallback text / localization key (bounded)
    FieldId field = kInvalidFieldId;
    IssueSeverity severity = IssueSeverity::Error;

    bool operator==(const FormIssue&) const = default;
};

enum class RuleKind : std::uint8_t {
    Required,
    MinLength,    // UTF-8 code points, string values only
    MaxLength,    // UTF-8 code points, string values only
    Pattern,      // documented glob: '*' any run, '?' one code point, other chars literal
    NumericRange, // integer range over a numeric string or the enum value
    EqualTo,      // value equals another field's current value
    Custom,       // caller callback returning nullopt or a stable code/message
};

struct RuleIssueTemplate {
    std::string code;
    std::string message;
};

// Custom synchronous rule. Must be pure/deterministic; it is called with immutable values and must
// not throw (a throw is mapped to the "custom_exception" issue instead of escaping the form).
using CustomSyncRule = std::function<std::optional<RuleIssueTemplate>(const FormValue&)>;

struct SyncRule {
    RuleKind kind = RuleKind::Required;
    std::int64_t minimum = 0;           // MinLength/MaxLength bounds, NumericRange minimum
    std::int64_t maximum = 0;           // NumericRange maximum
    std::string pattern;                // Pattern glob (bounded by FormConfig::max_pattern_bytes)
    FieldId equal_to = kInvalidFieldId; // EqualTo target
    CustomSyncRule custom;              // Custom rule
};

struct FormConfig {
    // Bounded form: add_field beyond max_fields returns nullopt.
    std::size_t max_fields = 32;
    // Bounded string values and names: longer values are rejected (set_value returns false), longer
    // names are truncated at a UTF-8 boundary. No per-keystroke allocation beyond the caller
    // string.
    std::size_t max_value_bytes = 4096;
    std::size_t max_name_bytes = 64;
    // Bounded rules/issues per field: extra rules are rejected; issues beyond the cap are counted
    // in FieldState::issue_count but not stored. max_form_issues bounds form-level issues.
    std::size_t max_rules_per_field = 8;
    std::size_t max_issues_per_field = 4;
    std::size_t max_form_issues = 8;
    std::size_t max_issue_code_bytes = 64;
    std::size_t max_issue_message_bytes = 128;
    std::size_t max_pattern_bytes = 128;
    // Values longer than this cannot be added; a name longer than max_name_bytes is truncated.
    std::size_t max_async_validators = 32;
    // Submit policy: by default every enabled field must be Valid and touched (platform-typical
    // form behavior; "touched" means the user interacted or blurred it).
    bool require_touched_for_submit = true;
    // Async validation trigger policy: default blur + submit (spec section 2); set true to also
    // validate asynchronously on every value change (debouncing is deferred).
    bool validate_async_on_change = false;
    // Queue used by async validators; required for async rules (nullptr => async rules are
    // ignored).
    tessera::runtime::CompletionQueue* completion_queue = nullptr;
};

// Read-only view of one field.
struct FieldState {
    FieldId id = kInvalidFieldId;
    std::string name;
    FormValue value;
    FormValue initial;
    bool dirty = false;
    bool touched = false;
    bool disabled = false;
    bool read_only = false;
    ValidationState state = ValidationState::Unknown;
    std::vector<FormIssue> issues; // first max_issues_per_field issues, deterministic order
    std::size_t issue_count = 0;   // total issues before the cap (>= issues.size())
    bool async_in_flight = false;

    [[nodiscard]] bool has_errors() const {
        return issue_count > 0;
    }
    [[nodiscard]] bool blocking() const {
        for (const FormIssue& issue : issues) {
            if (issue.severity == IssueSeverity::Error) {
                return true;
            }
        }
        return false;
    }
    bool operator==(const FieldState&) const = default;
};

enum class SubmitStatus : std::uint8_t {
    Ok,
    Invalid, // sync validation failed, a field is untouched, or the form is disabled
    Pending, // an async rule is still in flight
    Disabled,
};

struct SubmitResult {
    SubmitStatus status = SubmitStatus::Invalid;
    FieldId first_invalid = kInvalidFieldId; // Invalid: first blocking/untouched field
    std::size_t pending = 0;                 // Pending: fields still in flight
    bool operator==(const SubmitResult&) const = default;
};

} // namespace tessera::ui::forms
