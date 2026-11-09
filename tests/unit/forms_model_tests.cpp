// SPDX-License-Identifier: MIT
//
// Forms model (task-78): bounded field registry, sync rule matrix, touch/dirty policy, error
// ordering/caps, submit gating, reset, config/cap validation and determinism.

#include "tessera/ui/forms/form_model.hpp"

#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::forms::Form;
using tessera::ui::forms::FormConfig;
using tessera::ui::forms::FormIssue;
using tessera::ui::forms::FormValue;
using tessera::ui::forms::RuleIssueTemplate;
using tessera::ui::forms::RuleKind;
using tessera::ui::forms::SubmitResult;
using tessera::ui::forms::SubmitStatus;
using tessera::ui::forms::SyncRule;
using tessera::ui::forms::ValidationState;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "forms_model: " << message << "\n";
    }
    return condition;
}

SyncRule required_rule() {
    SyncRule rule;
    rule.kind = RuleKind::Required;
    return rule;
}

SyncRule length_rule(RuleKind kind, std::int64_t bound) {
    SyncRule rule;
    rule.kind = kind;
    if (kind == RuleKind::MaxLength) {
        rule.maximum = bound;
    } else {
        rule.minimum = bound;
    }
    return rule;
}

SyncRule pattern_rule(std::string pattern) {
    SyncRule rule;
    rule.kind = RuleKind::Pattern;
    rule.pattern = std::move(pattern);
    return rule;
}

SyncRule numeric_rule(std::int64_t minimum, std::int64_t maximum) {
    SyncRule rule;
    rule.kind = RuleKind::NumericRange;
    rule.minimum = minimum;
    rule.maximum = maximum;
    return rule;
}

SyncRule equal_rule(tessera::ui::forms::FieldId other) {
    SyncRule rule;
    rule.kind = RuleKind::EqualTo;
    rule.equal_to = other;
    return rule;
}

bool rule_passes(Form& form, tessera::ui::forms::FieldId id) {
    return form.field(id)->state == ValidationState::Valid;
}

bool rule_fails(Form& form, tessera::ui::forms::FieldId id, std::string_view code) {
    const auto* field = form.field(id);
    if (field == nullptr || field->state != ValidationState::Invalid) {
        return false;
    }
    for (const FormIssue& issue : field->issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

bool rule_matrix() {
    Form form;
    const auto text = form.add_field("text");
    const auto flag = form.add_field("flag", FormValue::from_bool(false));
    const auto choice = form.add_field("choice", FormValue::from_enum(-1));
    if (!expect(text && flag && choice, "fields added")) {
        return false;
    }
    form.add_rule(*text, required_rule());
    form.add_rule(*flag, required_rule());
    form.add_rule(*choice, required_rule());

    form.set_value(*text, FormValue::from_string(""));
    if (!expect(rule_fails(form, *text, "required"), "required rejects an empty string")) {
        return false;
    }
    form.set_value(*text, FormValue::from_string("x"));
    if (!expect(rule_passes(form, *text), "required accepts a non-empty string")) {
        return false;
    }
    if (!expect(rule_fails(form, *flag, "required"), "required rejects an unchecked bool")) {
        return false;
    }
    form.set_value(*flag, FormValue::from_bool(true));
    if (!expect(rule_passes(form, *flag), "required accepts a checked bool")) {
        return false;
    }
    if (!expect(rule_fails(form, *choice, "required"), "required rejects a negative enum")) {
        return false;
    }
    form.set_value(*choice, FormValue::from_enum(0));
    if (!expect(rule_passes(form, *choice), "required accepts enum 0")) {
        return false;
    }

    Form lengths;
    const auto length_field = lengths.add_field("length", FormValue::from_string("a"));
    lengths.add_rule(*length_field, length_rule(RuleKind::MinLength, 2));
    lengths.add_rule(*length_field, length_rule(RuleKind::MaxLength, 3));
    if (!expect(rule_fails(lengths, *length_field, "min_length"),
                "min length rejects short text")) {
        return false;
    }
    lengths.set_value(*length_field, FormValue::from_string("ab"));
    if (!expect(rule_passes(lengths, *length_field), "min length boundary passes")) {
        return false;
    }
    lengths.set_value(*length_field, FormValue::from_string("abcd"));
    if (!expect(rule_fails(lengths, *length_field, "max_length"), "max length rejects long text")) {
        return false;
    }
    lengths.set_value(*length_field, FormValue::from_string("\xF0\x9F\x98\x80\xF0\x9F\x98\x80"));
    if (!expect(rule_passes(lengths, *length_field), "length counts code points, not bytes")) {
        return false;
    }

    Form patterns;
    const auto pattern_field = patterns.add_field("pattern", FormValue::from_string("abc"));
    patterns.add_rule(*pattern_field, pattern_rule("a?c"));
    if (!expect(rule_passes(patterns, *pattern_field), "pattern: ? matches one code point")) {
        return false;
    }
    patterns.set_value(*pattern_field, FormValue::from_string("ac"));
    if (!expect(rule_fails(patterns, *pattern_field, "pattern"), "pattern: missing code point")) {
        return false;
    }
    patterns.set_value(*pattern_field, FormValue::from_string("abbbc"));
    // Replacing the rule set is not supported: add a wildcard field instead.
    Form wildcard;
    const auto star = wildcard.add_field("star", FormValue::from_string("abbbc"));
    wildcard.add_rule(*star, pattern_rule("a*c"));
    if (!expect(rule_passes(wildcard, *star), "pattern: * matches a run")) {
        return false;
    }
    wildcard.set_value(*star, FormValue::from_string("ab"));
    if (!expect(rule_fails(wildcard, *star, "pattern"), "pattern: missing trailing literal")) {
        return false;
    }
    Form unicode_pattern;
    const auto emoji =
        unicode_pattern.add_field("emoji", FormValue::from_string("\xF0\x9F\x98\x80"));
    unicode_pattern.add_rule(*emoji, pattern_rule("?"));
    if (!expect(rule_passes(unicode_pattern, *emoji), "pattern: ? matches an emoji code point")) {
        return false;
    }

    Form numeric;
    const auto number = numeric.add_field("number", FormValue::from_string("5"));
    numeric.add_rule(*number, numeric_rule(1, 10));
    if (!expect(rule_passes(numeric, *number), "numeric: in range")) {
        return false;
    }
    numeric.set_value(*number, FormValue::from_string("1"));
    if (!expect(rule_passes(numeric, *number), "numeric: lower boundary")) {
        return false;
    }
    numeric.set_value(*number, FormValue::from_string("11"));
    if (!expect(rule_fails(numeric, *number, "numeric_range"), "numeric: above range")) {
        return false;
    }
    numeric.set_value(*number, FormValue::from_string("-1"));
    if (!expect(rule_fails(numeric, *number, "numeric_range"), "numeric: below range")) {
        return false;
    }
    numeric.set_value(*number, FormValue::from_string("x"));
    if (!expect(rule_fails(numeric, *number, "not_numeric"), "numeric: not a number")) {
        return false;
    }
    Form enum_range;
    const auto level = enum_range.add_field("level", FormValue::from_enum(3));
    enum_range.add_rule(*level, numeric_rule(1, 5));
    if (!expect(rule_passes(enum_range, *level), "numeric: enum range")) {
        return false;
    }

    Form equal_form;
    const auto password = equal_form.add_field("password", FormValue::from_string("secret"));
    const auto confirm = equal_form.add_field("confirm", FormValue::from_string("secret"));
    if (!expect(equal_form.add_rule(*confirm, equal_rule(*password)), "equal-to rule added") ||
        !expect(rule_passes(equal_form, *confirm), "equal-to: equal values pass")) {
        return false;
    }
    equal_form.set_value(*confirm, FormValue::from_string("other"));
    if (!expect(rule_fails(equal_form, *confirm, "equal_to"), "equal-to: different values fail")) {
        return false;
    }
    equal_form.set_value(*password, FormValue::from_string("other"));
    if (!expect(rule_passes(equal_form, *confirm), "equal-to: target change re-evaluates")) {
        return false;
    }

    Form custom_form;
    const auto custom = custom_form.add_field("custom", FormValue::from_string("ok"));
    SyncRule custom_rule;
    custom_rule.kind = RuleKind::Custom;
    custom_rule.custom = [](const FormValue& value) -> std::optional<RuleIssueTemplate> {
        if (value.text == "ok") {
            return std::nullopt;
        }
        return RuleIssueTemplate{"custom_bad", "Bad value"};
    };
    custom_form.add_rule(*custom, custom_rule);
    if (!expect(rule_passes(custom_form, *custom), "custom: valid value")) {
        return false;
    }
    custom_form.set_value(*custom, FormValue::from_string("bad"));
    if (!expect(rule_fails(custom_form, *custom, "custom_bad"), "custom: issue code")) {
        return false;
    }
    SyncRule throwing_rule;
    throwing_rule.kind = RuleKind::Custom;
    throwing_rule.custom = [](const FormValue&) -> std::optional<RuleIssueTemplate> {
        throw std::runtime_error("boom");
    };
    Form throwing_form;
    const auto throwing = throwing_form.add_field("throwing", FormValue::from_string("x"));
    throwing_form.add_rule(*throwing, throwing_rule);
    if (!expect(rule_fails(throwing_form, *throwing, "custom_exception"),
                "custom: a throw becomes an issue")) {
        return false;
    }
    Form wrong_kind;
    const auto text_field = wrong_kind.add_field("text", FormValue::from_string("x"));
    wrong_kind.add_rule(*text_field, length_rule(RuleKind::MinLength, 1));
    return expect(rule_passes(wrong_kind, *text_field), "length rule for a string field passes");
}

bool policy_and_flags() {
    Form form;
    const auto field = form.add_field("name", FormValue::from_string("Ada"));
    if (!expect(form.field(*field)->state == ValidationState::Unknown,
                "a new field starts Unknown") ||
        !expect(!form.field(*field)->touched && !form.field(*field)->dirty,
                "a new field is untouched and clean")) {
        return false;
    }
    form.set_value(*field, FormValue::from_string("Grace"));
    if (!expect(form.field(*field)->state == ValidationState::Valid, "set_value validates sync") ||
        !expect(form.field(*field)->dirty, "set_value marks dirty") ||
        !expect(!form.field(*field)->touched, "set_value does not touch")) {
        return false;
    }
    form.set_value(*field, FormValue::from_string("Ada"));
    if (!expect(!form.field(*field)->dirty, "restoring the initial value clears dirty")) {
        return false;
    }
    form.touch(*field);
    if (!expect(form.field(*field)->touched, "touch marks touched")) {
        return false;
    }
    form.reset_field(*field);
    if (!expect(!form.field(*field)->touched && !form.field(*field)->dirty &&
                    form.field(*field)->state == ValidationState::Unknown,
                "reset_field clears flags/state")) {
        return false;
    }
    form.set_value(*field, FormValue::from_string("Grace"));
    form.reset();
    return expect(form.field(*field)->value.text == "Ada", "reset restores the initial value") &&
           expect(!form.field(*field)->dirty, "reset clears dirty");
}

bool error_caps_and_ordering() {
    FormConfig config;
    config.max_issues_per_field = 2;
    Form form(config);
    const auto field = form.add_field("field", FormValue::from_string(""));
    form.add_rule(*field, required_rule());
    form.add_rule(*field, length_rule(RuleKind::MinLength, 3));
    SyncRule numeric = numeric_rule(1, 10);
    form.add_rule(*field, numeric);
    SyncRule pattern = pattern_rule("abc");
    form.add_rule(*field, pattern);
    form.set_value(*field, FormValue::from_string(""));
    const auto* state = form.field(*field);
    if (!expect(state->issues.size() == 2, "issues are capped at max_issues_per_field") ||
        !expect(state->issue_count == 4, "the count keeps the total before the cap") ||
        !expect(state->issues[0].code == "required", "errors keep rule order") ||
        !expect(state->issues[1].code == "min_length", "second error follows registration order")) {
        return false;
    }
    return expect(state->has_errors() && state->blocking(), "capped errors still block");
}

bool caps_and_config() {
    FormConfig config;
    config.max_fields = 2;
    config.max_name_bytes = 4;
    config.max_value_bytes = 8;
    config.max_rules_per_field = 2;
    Form form(config);
    if (!expect(form.add_field("a").has_value(), "first field") ||
        !expect(form.add_field("b").has_value(), "second field") ||
        !expect(!form.add_field("c").has_value(), "field cap rejects the third field") ||
        !expect(!form.add_field("").has_value(), "empty name rejected") ||
        !expect(!form.add_field("a").has_value(), "duplicate name rejected")) {
        return false;
    }
    Form truncated(config);
    const auto long_name = truncated.add_field("abcdefgh");
    if (!expect(long_name.has_value() && truncated.field(*long_name)->name.size() == 4,
                "long names truncate at the byte cap")) {
        return false;
    }
    if (!expect(!truncated.set_value(*long_name, FormValue::from_string("123456789")),
                "oversized values are rejected") ||
        !expect(truncated.set_value(*long_name, FormValue::from_string("12345678")),
                "the exact value bound is accepted")) {
        return false;
    }
    const auto rule_field = truncated.add_field("r", FormValue::from_string(""));
    if (!expect(truncated.add_rule(*rule_field, required_rule()), "first rule") ||
        !expect(truncated.add_rule(*rule_field, pattern_rule("?")), "second rule") ||
        !expect(!truncated.add_rule(*rule_field, pattern_rule("*")), "rule cap rejects")) {
        return false;
    }
    if (!expect(!truncated.add_rule(*rule_field, equal_rule(99)), "equal-to needs a target")) {
        return false;
    }
    bool threw = false;
    try {
        FormConfig bad;
        bad.max_fields = 0;
        Form invalid(bad);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero field cap throws")) {
        return false;
    }
    Form disabled_form;
    const auto disabled = disabled_form.add_field("disabled", FormValue::from_string("x"), true);
    return expect(!disabled_form.set_value(*disabled, FormValue::from_string("y")),
                  "disabled fields reject values") &&
           expect(disabled_form.valid(), "disabled fields do not block validity");
}

bool submit_paths() {
    Form form;
    const auto name = form.add_field("name", FormValue::from_string(""));
    const auto email = form.add_field("email", FormValue::from_string("a@b.c"));
    form.add_rule(*name, required_rule());
    if (!expect(form.submit().status == SubmitStatus::Invalid, "untouched required field blocks") ||
        !expect(form.submit().first_invalid == *name, "first invalid is the required field")) {
        return false;
    }
    form.touch(*name);
    if (!expect(form.submit().status == SubmitStatus::Invalid, "empty touched field blocks")) {
        return false;
    }
    form.set_value(*name, FormValue::from_string("Ada"));
    form.touch(*email);
    if (!expect(form.can_submit(), "valid + touched fields can submit")) {
        return false;
    }
    const SubmitResult ok = form.submit();
    if (!expect(ok.status == SubmitStatus::Ok && ok.first_invalid == 0,
                "submit succeeds when every enabled field is Valid+touched")) {
        return false;
    }
    form.set_value(*email, FormValue::from_string(""));
    form.add_rule(*email, required_rule());
    const SubmitResult invalid = form.submit();
    return expect(invalid.status == SubmitStatus::Invalid && invalid.first_invalid == *email,
                  "submit reports the first invalid field") &&
           expect(!form.can_submit(), "can_submit reflects the invalid field");
}

bool determinism() {
    const auto build = [] {
        Form form;
        const auto name = form.add_field("name", FormValue::from_string(""));
        form.add_rule(*name, required_rule());
        form.add_rule(*name, length_rule(RuleKind::MaxLength, 8));
        form.set_value(*name, FormValue::from_string("Ada"));
        form.touch(*name);
        return *form.field(*name);
    };
    return expect(build() == build(), "the same script produces identical field state");
}

} // namespace

int main() {
    if (!rule_matrix()) {
        return 1;
    }
    if (!policy_and_flags()) {
        return 2;
    }
    if (!error_caps_and_ordering()) {
        return 3;
    }
    if (!caps_and_config()) {
        return 4;
    }
    if (!submit_paths()) {
        return 5;
    }
    if (!determinism()) {
        return 6;
    }

    std::cout << "forms_model: ok\n";
    return 0;
}
