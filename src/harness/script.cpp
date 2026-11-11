// SPDX-License-Identifier: MIT

#include "script_internal.hpp"
#include "tessera/harness/json_parse.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::harness {

std::string_view to_string(ScriptAction action) noexcept {
    switch (action) {
    case ScriptAction::Click:
        return kClickCommand;
    case ScriptAction::Type:
        return "type";
    case ScriptAction::Focus:
        return kFocusCommand;
    case ScriptAction::Scroll:
        return kScrollCommand;
    case ScriptAction::Snapshot:
        return kSnapshotCommand;
    case ScriptAction::Trace:
        return kTraceCommand;
    case ScriptAction::Audit:
        return kAuditCommand;
    }
    return kClickCommand;
}

std::optional<ScriptAction> script_action_from_string(std::string_view text) noexcept {
    if (text == kClickCommand) {
        return ScriptAction::Click;
    }
    if (text == "type") {
        return ScriptAction::Type;
    }
    if (text == kFocusCommand) {
        return ScriptAction::Focus;
    }
    if (text == kScrollCommand) {
        return ScriptAction::Scroll;
    }
    if (text == kSnapshotCommand) {
        return ScriptAction::Snapshot;
    }
    if (text == kTraceCommand) {
        return ScriptAction::Trace;
    }
    if (text == kAuditCommand) {
        return ScriptAction::Audit;
    }
    return std::nullopt;
}

bool is_spec_script_action(std::string_view text) noexcept {
    constexpr std::array<std::string_view, 23> kActions = {
        "launch",       "close",      "resize",         "click",       "type",
        "key",          "scroll",     "focus",          "waitFor",     "assert",
        "snapshot",     "screenshot", "trace",          "audit",       "custom",
        "assertCustom", "navigate",   "back",           "openOverlay", "closeOverlay",
        "assertRoute",  "assertText", "assertSelection"};
    for (const std::string_view action : kActions) {
        if (action == text) {
            return true;
        }
    }
    return false;
}

std::string_view to_string(ScriptErrorCode code) noexcept {
    switch (code) {
    case ScriptErrorCode::MalformedScript:
        return "malformedScript";
    case ScriptErrorCode::UnsupportedVersion:
        return "unsupportedVersion";
    case ScriptErrorCode::UnknownAction:
        return "unknownAction";
    case ScriptErrorCode::UnsupportedAction:
        return "unsupportedAction";
    case ScriptErrorCode::InvalidStep:
        return "invalidStep";
    case ScriptErrorCode::ScriptTooLarge:
        return "scriptTooLarge";
    case ScriptErrorCode::StepCapExceeded:
        return "stepCapExceeded";
    case ScriptErrorCode::StepTimeout:
        return "stepTimeout";
    case ScriptErrorCode::RunTimeout:
        return "runTimeout";
    case ScriptErrorCode::Cancelled:
        return "cancelled";
    case ScriptErrorCode::HandlerError:
        return "handlerError";
    case ScriptErrorCode::ArtifactError:
        return "artifactError";
    case ScriptErrorCode::Internal:
        return "internal";
    }
    return "internal";
}

std::expected<Script, ScriptError> parse_script(std::string_view text) {
    if (text.size() > kMaxScriptBytes) {
        return std::unexpected(detail::script_error(ScriptErrorCode::ScriptTooLarge,
                                                    "script exceeds the 1 MiB cap",
                                                    std::to_string(text.size()) + " bytes"));
    }
    auto document = parse_json(text);
    if (!document.has_value()) {
        return std::unexpected(
            detail::script_error(ScriptErrorCode::MalformedScript, "script is not valid JSON",
                                 "offset " + std::to_string(document.error().offset)));
    }
    if (!document->is_object()) {
        return std::unexpected(detail::script_error(ScriptErrorCode::MalformedScript,
                                                    "script document must be a JSON object"));
    }
    if (auto error = detail::reject_unknown(*document, {"scriptVersion", "profile", "app", "steps"},
                                            kNoScriptStep, "script");
        error.has_value()) {
        return std::unexpected(std::move(*error));
    }

    const JsonValue* version = document->find("scriptVersion");
    const auto version_value = version == nullptr ? std::nullopt : version->as_integer();
    if (!version_value.has_value()) {
        return std::unexpected(detail::script_error(
            ScriptErrorCode::MalformedScript, "scriptVersion is required and must be an integer",
            "scriptVersion"));
    }
    if (*version_value != 1) {
        return std::unexpected(detail::script_error(ScriptErrorCode::UnsupportedVersion,
                                                    "unsupported script version",
                                                    std::to_string(*version_value)));
    }

    Script script;
    script.version = *version_value;

    if (const JsonValue* profile = document->find("profile"); profile != nullptr) {
        auto parsed = detail::parse_script_profile(*profile);
        if (!parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
        script.profile = std::move(*parsed);
    }
    if (const JsonValue* app = document->find("app"); app != nullptr) {
        auto parsed = detail::parse_script_app(*app);
        if (!parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
        script.app_config = std::move(*parsed);
    }

    const JsonValue* steps = document->find("steps");
    if (steps == nullptr || !steps->is_array()) {
        return std::unexpected(detail::script_error(
            ScriptErrorCode::MalformedScript, "steps is required and must be an array", "steps"));
    }
    if (steps->size() > kMaxScriptSteps) {
        return std::unexpected(detail::script_error(ScriptErrorCode::StepCapExceeded,
                                                    "script has more steps than the cap",
                                                    std::to_string(steps->size())));
    }
    script.steps.reserve(steps->size());
    for (std::size_t index = 0; index < steps->size(); ++index) {
        auto parsed = detail::parse_script_step((*steps->as_array())[index], index);
        if (!parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
        script.steps.push_back(std::move(*parsed));
    }
    return script;
}

} // namespace tessera::harness
