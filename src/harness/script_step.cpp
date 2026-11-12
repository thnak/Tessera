// SPDX-License-Identifier: MIT

#include "script_internal.hpp"
#include "tessera/harness/artifact_sink.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::harness::detail {
namespace {

std::unexpected<ScriptError> unexpected(ScriptError error) {
    return std::unexpected(std::move(error));
}

std::optional<std::uint32_t> read_dimension(const JsonValue* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto integer = value->as_integer();
    if (!integer.has_value() || *integer <= 0 ||
        *integer > static_cast<std::int64_t>(kMaxScriptProfileDimension)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*integer);
}

} // namespace

ScriptError script_error(ScriptErrorCode code, std::string message, std::string detail,
                         std::size_t step) {
    ScriptError error;
    error.code = code;
    error.message = std::move(message);
    error.detail = std::move(detail);
    error.step = step;
    return error;
}

std::optional<ScriptError> reject_unknown(const JsonValue& value,
                                          std::initializer_list<std::string_view> allowed,
                                          std::size_t step, std::string_view what) {
    const JsonValue::Object* members = value.as_object();
    if (members == nullptr) {
        return script_error(ScriptErrorCode::MalformedScript,
                            std::string(what) + " must be an object", {}, step);
    }
    for (const JsonValue::Member& member : *members) {
        bool known = false;
        for (const std::string_view key : allowed) {
            known = known || key == member.first;
        }
        if (!known) {
            return script_error(ScriptErrorCode::MalformedScript,
                                std::string(what) + " has an unknown member", member.first, step);
        }
    }
    return std::nullopt;
}

std::optional<std::string> read_string(const JsonValue* value, std::size_t max_bytes) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto text = value->as_string();
    if (!text.has_value() || text->size() > max_bytes) {
        return std::nullopt;
    }
    return std::string(*text);
}

std::expected<ScriptProfile, ScriptError> parse_script_profile(const JsonValue& value) {
    if (auto error =
            reject_unknown(value, {"width", "height", "scale", "locale"}, kNoScriptStep, "profile");
        error.has_value()) {
        return unexpected(std::move(*error));
    }
    ScriptProfile profile;
    if (const JsonValue* width = value.find("width"); width != nullptr) {
        const auto parsed = read_dimension(width);
        if (!parsed.has_value()) {
            return unexpected(script_error(ScriptErrorCode::MalformedScript,
                                           "profile.width must be an integer in 1..16384",
                                           "width"));
        }
        profile.width = *parsed;
    }
    if (const JsonValue* height = value.find("height"); height != nullptr) {
        const auto parsed = read_dimension(height);
        if (!parsed.has_value()) {
            return unexpected(script_error(ScriptErrorCode::MalformedScript,
                                           "profile.height must be an integer in 1..16384",
                                           "height"));
        }
        profile.height = *parsed;
    }
    if (const JsonValue* scale = value.find("scale"); scale != nullptr) {
        const auto parsed = scale->as_number();
        if (!parsed.has_value() || !std::isfinite(*parsed) || !(*parsed > 0.0) ||
            *parsed > kMaxScriptScale) {
            return unexpected(script_error(ScriptErrorCode::MalformedScript,
                                           "profile.scale must be a finite number in (0, 8]",
                                           "scale"));
        }
        profile.scale = *parsed;
    }
    if (const JsonValue* locale = value.find("locale"); locale != nullptr) {
        const auto parsed = read_string(locale, kMaxScriptLocaleBytes);
        if (!parsed.has_value() || parsed->empty()) {
            return unexpected(script_error(
                ScriptErrorCode::MalformedScript,
                "profile.locale must be a non-empty string of at most 64 bytes", "locale"));
        }
        profile.locale = std::move(*parsed);
    }
    return profile;
}

std::expected<std::string, ScriptError> parse_script_app(const JsonValue& value) {
    if (auto error = reject_unknown(value, {"config"}, kNoScriptStep, "app"); error.has_value()) {
        return unexpected(std::move(*error));
    }
    const JsonValue* config = value.find("config");
    if (config == nullptr) {
        return std::string{};
    }
    const auto parsed = read_string(config, kMaxArtifactPathBytes);
    if (!parsed.has_value() || !is_safe_artifact_path(*parsed)) {
        return unexpected(script_error(ScriptErrorCode::MalformedScript,
                                       "app.config must be a safe relative path", "config"));
    }
    return std::move(*parsed);
}

std::expected<ScriptStep, ScriptError> parse_script_step(const JsonValue& value,
                                                         std::size_t index) {
    if (!value.is_object()) {
        return unexpected(
            script_error(ScriptErrorCode::InvalidStep, "step must be an object", {}, index));
    }
    const JsonValue* action_value = value.find("action");
    const auto action_name = action_value == nullptr ? std::nullopt : action_value->as_string();
    if (!action_name.has_value() || action_name->empty()) {
        return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                       "step.action must be a non-empty string", "action", index));
    }
    const auto action = script_action_from_string(*action_name);
    if (!action.has_value()) {
        if (is_spec_script_action(*action_name)) {
            return unexpected(script_error(ScriptErrorCode::UnsupportedAction,
                                           "script action is not implemented in v1",
                                           std::string(*action_name), index));
        }
        return unexpected(script_error(ScriptErrorCode::UnknownAction, "unknown script action",
                                       std::string(*action_name), index));
    }

    const bool has_ui_id = *action == ScriptAction::Click || *action == ScriptAction::Type ||
                           *action == ScriptAction::Focus || *action == ScriptAction::Scroll ||
                           *action == ScriptAction::ScrollIntoView ||
                           *action == ScriptAction::Assert || *action == ScriptAction::WaitFor;
    const bool is_assert = *action == ScriptAction::Assert;
    const bool is_wait = *action == ScriptAction::WaitFor;
    const bool is_scroll_into_view = *action == ScriptAction::ScrollIntoView;
    const bool has_text = *action == ScriptAction::Type;
    const bool has_scroll = *action == ScriptAction::Scroll;
    const bool has_path = *action == ScriptAction::Snapshot || *action == ScriptAction::Trace ||
                          *action == ScriptAction::Audit;
    const bool has_profile = *action == ScriptAction::Audit;

    const JsonValue::Object* members = value.as_object();
    for (const JsonValue::Member& member : *members) {
        const std::string_view key = member.first;
        const bool known = key == "action" || key == "timeoutMs" ||
                           (has_ui_id && (key == "uiId" || key == "itemKey")) ||
                           (is_scroll_into_view && (key == "itemIndex" || key == "alignment")) ||
                           (has_text && key == "text") ||
                           (has_scroll && (key == "dx" || key == "dy")) ||
                           (has_path && key == "path") || (has_profile && key == "profile") ||
                           (*action == ScriptAction::Snapshot && key == "includeHidden") ||
                           (is_wait && key == "state") ||
                           (is_assert && (key == "exists" || key == "visible" || key == "enabled" ||
                                          key == "focused" || key == "selected" || key == "text" ||
                                          key == "name"));
        if (!known) {
            return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                           "step has an unknown member", std::string(key), index));
        }
    }

    ScriptStep step;
    step.action = *action;

    if (has_ui_id) {
        const JsonValue* ui_id = value.find("uiId");
        const auto text = ui_id == nullptr ? std::nullopt : ui_id->as_string();
        UiId parsed = 0;
        if (!text.has_value() || !parse_decimal_id(*text, parsed)) {
            return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                           "step.uiId must be an unsigned decimal string", "uiId",
                                           index));
        }
        step.ui_id = parsed;
        if (const JsonValue* item_key = value.find("itemKey"); item_key != nullptr) {
            const auto key = read_string(item_key, kMaxScriptItemKeyBytes);
            if (!key.has_value() || key->empty()) {
                return unexpected(
                    script_error(ScriptErrorCode::InvalidStep,
                                 "step.itemKey must be a non-empty string of at most 256 bytes",
                                 "itemKey", index));
            }
            step.item_key = std::move(*key);
        }
        if (is_scroll_into_view) {
            if (const JsonValue* item_index = value.find("itemIndex"); item_index != nullptr) {
                const auto parsed = item_index->as_integer();
                if (!parsed.has_value() || *parsed < 0 ||
                    static_cast<std::uint64_t>(*parsed) > std::numeric_limits<std::size_t>::max()) {
                    return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                                   "step.itemIndex must be a non-negative integer",
                                                   "itemIndex", index));
                }
                step.item_index = static_cast<std::size_t>(*parsed);
            }
            if (const JsonValue* alignment = value.find("alignment"); alignment != nullptr) {
                const auto text = read_string(alignment, 16);
                const auto parsed =
                    text.has_value() ? scroll_alignment_from_string(*text) : std::nullopt;
                if (!parsed.has_value()) {
                    return unexpected(
                        script_error(ScriptErrorCode::InvalidStep,
                                     "step.alignment must be start, center, end, or nearest",
                                     "alignment", index));
                }
                step.alignment = *parsed;
            }
            if (step.item_key.has_value() && step.item_index.has_value()) {
                return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                               "step.itemKey and step.itemIndex are mutually "
                                               "exclusive",
                                               "itemKey", index));
            }
            if (value.find("alignment") != nullptr && !step.item_index.has_value()) {
                return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                               "step.alignment requires step.itemIndex",
                                               "alignment", index));
            }
        }
    }

    if (has_text) {
        const JsonValue* text = value.find("text");
        const auto parsed = read_string(text, kMaxScriptTextBytes);
        if (!parsed.has_value()) {
            return unexpected(script_error(
                ScriptErrorCode::InvalidStep,
                "step.text is required and must be a string of at most 64 KiB", "text", index));
        }
        step.text = std::move(*parsed);
    }

    if (is_wait) {
        const JsonValue* state_value = value.find("state");
        std::string state = "visible";
        if (state_value != nullptr) {
            const auto parsed = read_string(state_value, 16);
            if (!parsed.has_value()) {
                return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                               "step.state must be a string", "state", index));
            }
            state = *parsed;
        }
        if (state == "exists") {
            step.expect_exists = true;
        } else if (state == "visible") {
            step.expect_visible = true;
        } else if (state == "enabled") {
            step.expect_enabled = true;
        } else if (state == "focused") {
            step.expect_focused = true;
        } else if (state == "selected") {
            step.expect_selected = true;
        } else {
            return unexpected(
                script_error(ScriptErrorCode::InvalidStep,
                             "step.state must be exists, visible, enabled, focused or selected",
                             "state", index));
        }
    }

    if (is_assert) {
        const std::pair<std::string_view, std::optional<bool>*> flags[] = {
            {"exists", &step.expect_exists},
            {"visible", &step.expect_visible},
            {"enabled", &step.expect_enabled},
            {"focused", &step.expect_focused},
            {"selected", &step.expect_selected}};
        for (const auto& [name, target] : flags) {
            if (const JsonValue* flag = value.find(name); flag != nullptr) {
                const auto parsed = flag->as_bool();
                if (!parsed.has_value()) {
                    return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                                   "assert expectation must be a boolean",
                                                   std::string(name), index));
                }
                *target = *parsed;
            }
        }
        const std::pair<std::string_view, std::optional<std::string>*> texts[] = {
            {"text", &step.expect_text}, {"name", &step.expect_name}};
        for (const auto& [name, target] : texts) {
            if (const JsonValue* member = value.find(name); member != nullptr) {
                auto parsed = read_string(member, kMaxScriptTextBytes);
                if (!parsed.has_value()) {
                    return unexpected(
                        script_error(ScriptErrorCode::InvalidStep,
                                     "assert text/name must be a string of at most 64 KiB",
                                     std::string(name), index));
                }
                *target = std::move(*parsed);
            }
        }
        const bool any = step.expect_exists || step.expect_visible || step.expect_enabled ||
                         step.expect_focused || step.expect_selected || step.expect_text ||
                         step.expect_name;
        if (!any) {
            return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                           "assert needs at least one expectation", "assert",
                                           index));
        }
    }

    if (has_scroll) {
        const JsonValue* dx = value.find("dx");
        const JsonValue* dy = value.find("dy");
        const auto dx_value = dx == nullptr ? std::nullopt : dx->as_number();
        const auto dy_value = dy == nullptr ? std::nullopt : dy->as_number();
        if (!dx_value.has_value() || !std::isfinite(*dx_value)) {
            return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                           "step.dx is required and must be finite", "dx", index));
        }
        if (!dy_value.has_value() || !std::isfinite(*dy_value)) {
            return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                           "step.dy is required and must be finite", "dy", index));
        }
        step.scroll_dx = *dx_value;
        step.scroll_dy = *dy_value;
    }

    if (has_path) {
        if (const JsonValue* path = value.find("path"); path != nullptr) {
            const auto parsed = read_string(path, kMaxArtifactPathBytes);
            if (!parsed.has_value() || !is_safe_artifact_path(*parsed)) {
                return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                               "step.path must be a safe relative artifact path",
                                               "path", index));
            }
            step.path = std::move(*parsed);
        }
    }

    if (*action == ScriptAction::Snapshot) {
        if (const JsonValue* include_hidden = value.find("includeHidden");
            include_hidden != nullptr) {
            const auto parsed = include_hidden->as_bool();
            if (!parsed.has_value()) {
                return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                               "step.includeHidden must be a boolean",
                                               "includeHidden", index));
            }
            step.include_hidden = *parsed;
        }
    }

    if (has_profile) {
        const JsonValue* profile = value.find("profile");
        const auto parsed = read_string(profile, kMaxScriptProfileNameBytes);
        if (!parsed.has_value() || parsed->empty()) {
            return unexpected(
                script_error(ScriptErrorCode::InvalidStep,
                             "step.profile is required for audit and must be a non-empty string",
                             "profile", index));
        }
        step.profile = std::move(*parsed);
    }

    if (const JsonValue* timeout = value.find("timeoutMs"); timeout != nullptr) {
        const auto parsed = timeout->as_integer();
        if (!parsed.has_value() || *parsed < 1 ||
            *parsed > static_cast<std::int64_t>(kMaxScriptStepTimeoutMs)) {
            return unexpected(script_error(ScriptErrorCode::InvalidStep,
                                           "step.timeoutMs must be an integer in 1..60000",
                                           "timeoutMs", index));
        }
        step.timeout_us = static_cast<std::uint64_t>(*parsed) * 1000U;
    }

    return step;
}

} // namespace tessera::harness::detail
