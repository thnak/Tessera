// SPDX-License-Identifier: MIT

#include "tessera/harness/script.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::harness::is_spec_script_action;
using tessera::harness::kMaxScriptBytes;
using tessera::harness::kMaxScriptSteps;
using tessera::harness::kNoScriptStep;
using tessera::harness::parse_script;
using tessera::harness::Script;
using tessera::harness::script_action_from_string;
using tessera::harness::ScriptAction;
using tessera::harness::ScriptErrorCode;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "script: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string step_document(std::string_view step) {
    return std::string(R"({"scriptVersion":1,"steps":[)") + std::string(step) + "]}";
}

ScriptErrorCode parse_error(std::string_view json, std::size_t* step = nullptr) {
    const auto parsed = parse_script(json);
    if (parsed.has_value()) {
        return ScriptErrorCode::MalformedScript; // sentinel: the document should not parse
    }
    if (step != nullptr) {
        *step = parsed.error().step;
    }
    return parsed.error().code;
}

} // namespace

int main() {
    // A complete section 4 document decodes field by field, including the narrow choices.
    {
        constexpr std::string_view kDocument = R"({
            "scriptVersion": 1,
            "profile": {"width": 1024, "height": 768, "scale": 2.0, "locale": "en-GB"},
            "app": {"config": "tessera.json"},
            "steps": [
                {"action": "click", "uiId": "42"},
                {"action": "type", "uiId": "7", "text": "alice",
                 "itemKey": "row-1", "timeoutMs": 250},
                {"action": "focus", "uiId": "42", "itemKey": "row-1"},
                {"action": "scroll", "uiId": "5", "dx": 0, "dy": 120.5},
                {"action": "snapshot", "includeHidden": true, "path": "out/snapshot.json"},
                {"action": "trace", "path": "out/trace.json"},
                {"action": "audit", "profile": "desktop"}
            ]
        })";
        const auto parsed = parse_script(kDocument);
        expect(parsed.has_value(), "happy-path document parses");
        const Script& script = *parsed;
        expect(script.version == 1, "version decoded");
        expect(script.profile.width == 1024 && script.profile.height == 768, "profile size");
        expect(script.profile.scale == 2.0, "profile scale");
        expect(script.profile.locale == "en-GB", "profile locale");
        expect(script.app_config == "tessera.json", "app config");
        expect(script.steps.size() == 7, "seven steps");
        expect(script.steps[0].action == ScriptAction::Click && script.steps[0].ui_id == 42,
               "click target");
        expect(script.steps[1].action == ScriptAction::Type, "type action");
        expect(script.steps[1].text == "alice", "type text");
        expect(script.steps[1].item_key == std::optional<std::string>("row-1"), "item key");
        expect(script.steps[1].timeout_us == 250'000, "timeoutMs converted to microseconds");
        expect(script.steps[2].item_key.has_value(), "focus keeps its item key");
        expect(script.steps[3].scroll_dy == 120.5 && script.steps[3].scroll_dx == 0.0,
               "scroll offsets");
        expect(script.steps[4].include_hidden && script.steps[4].path == "out/snapshot.json",
               "snapshot fields");
        expect(script.steps[5].path == "out/trace.json", "trace path");
        expect(script.steps[6].profile == "desktop" && script.steps[6].path.empty(),
               "audit profile and default path");
    }

    // Defaults and an empty step list are valid; an absent timeout means "use the budget".
    {
        const auto parsed = parse_script(R"({"scriptVersion":1,"steps":[{"action":"snapshot"}]})");
        expect(parsed.has_value(), "minimal document parses");
        expect(parsed->profile.width == 1280 && parsed->profile.height == 800,
               "default profile matches the section 4 example");
        expect(parsed->profile.scale == 1.0 && parsed->profile.locale == "en-US",
               "default scale/locale");
        expect(parsed->steps[0].timeout_us == 0, "no timeout override");
        expect(parsed->steps[0].path.empty(), "snapshot without a path embeds its result");
        const auto empty = parse_script(R"({"scriptVersion":1,"steps":[]})");
        expect(empty.has_value() && empty->steps.empty(), "empty step list is valid");
    }

    // Version handling is strict: required, integral, and exactly 1.
    {
        expect(parse_error(R"({"steps":[]})") == ScriptErrorCode::MalformedScript,
               "missing scriptVersion");
        expect(parse_error(R"({"scriptVersion":"1","steps":[]})") ==
                   ScriptErrorCode::MalformedScript,
               "string version rejected");
        expect(parse_error(R"({"scriptVersion":2,"steps":[]})") ==
                   ScriptErrorCode::UnsupportedVersion,
               "future version rejected");
        expect(parse_error(R"({"scriptVersion":1,"steps":[],"extra":true})") ==
                   ScriptErrorCode::MalformedScript,
               "unknown top-level member rejected");
        expect(parse_error(R"({"scriptVersion":1,"steps":{}})") == ScriptErrorCode::MalformedScript,
               "steps must be an array");
        expect(parse_error("not json") == ScriptErrorCode::MalformedScript, "invalid JSON");
    }

    // The parser bounds the document and the step count before any handler exists.
    {
        expect(parse_error(std::string(kMaxScriptBytes + 1, ' ')) ==
                   ScriptErrorCode::ScriptTooLarge,
               "oversized document rejected");
        std::string document = R"({"scriptVersion":1,"steps":[)";
        for (std::size_t index = 0; index <= kMaxScriptSteps; ++index) {
            if (index != 0) {
                document.push_back(',');
            }
            document += R"({"action":"click","uiId":"1"})";
        }
        document += "]}";
        expect(parse_error(document) == ScriptErrorCode::StepCapExceeded,
               "step count cap enforced at parse time");
    }

    // A section 4 action the v1 runner cannot execute is rejected as such, not as a typo.
    {
        std::size_t step = kNoScriptStep;
        expect(parse_error(step_document(R"({"action":"screenshot","path":"a.png"})"), &step) ==
                   ScriptErrorCode::UnsupportedAction,
               "screenshot is known but unsupported");
        expect(step == 0, "unsupported action carries the step index");
        expect(parse_error(step_document(R"({"action":"assert","uiId":"1"})")) ==
                   ScriptErrorCode::InvalidStep,
               "assert needs an expectation");
        expect(parse_error(step_document(R"({"action":"assert","uiId":"1","visible":"yes"})")) ==
                   ScriptErrorCode::InvalidStep,
               "assert flag must be boolean");
        expect(parse_error(step_document(R"({"action":"waitFor","uiId":"1","state":"gone"})")) ==
                   ScriptErrorCode::InvalidStep,
               "waitFor state is a closed set");
        {
            const auto parsed = parse_script(step_document(
                R"({"action":"assert","uiId":"3","itemKey":"k","text":"Hi","enabled":true})"));
            expect(parsed.has_value() && parsed->steps[0].action == ScriptAction::Assert &&
                       parsed->steps[0].expect_text == "Hi" &&
                       parsed->steps[0].expect_enabled == true && parsed->steps[0].item_key == "k",
                   "assert expectations decode");
            const auto wait = parse_script(step_document(R"({"action":"waitFor","uiId":"3"})"));
            expect(wait.has_value() && wait->steps[0].expect_visible == true,
                   "waitFor defaults to visible");
        }
        expect(parse_error(step_document(R"({"action":"frobnicate"})")) ==
                   ScriptErrorCode::UnknownAction,
               "unknown action");
        expect(parse_error(step_document(R"({"action":"setValue","uiId":"1"})")) ==
                   ScriptErrorCode::UnknownAction,
               "wire command names are not script actions");
        expect(is_spec_script_action("navigate") && !is_spec_script_action("setValue"),
               "spec action table is narrow");
        expect(script_action_from_string("type") == ScriptAction::Type, "type action spelling");
        expect(script_action_from_string("scrollIntoView") == ScriptAction::ScrollIntoView,
               "scrollIntoView action spelling");
    }

    // Malformed steps are rejected whole, with a stable code and the step index.
    {
        std::size_t step = kNoScriptStep;
        expect(parse_error(step_document(R"({"action":"click"})"), &step) ==
                   ScriptErrorCode::InvalidStep,
               "missing uiId");
        expect(step == 0, "missing uiId carries the step index");
        expect(parse_error(step_document(R"({"action":"click","uiId":42})")) ==
                   ScriptErrorCode::InvalidStep,
               "numeric uiId rejected (decimal string only)");
        expect(parse_error(step_document(R"({"action":"click","uiId":"042"})")) ==
                   ScriptErrorCode::InvalidStep,
               "non-canonical uiId rejected");
        expect(parse_error(step_document(R"({"action":"click","uiId":"18446744073709551616"})")) ==
                   ScriptErrorCode::InvalidStep,
               "uiId overflow rejected");
        expect(parse_error(step_document(R"({"action":"click","uiId":"1","wat":1})")) ==
                   ScriptErrorCode::InvalidStep,
               "unknown step member rejected");
        expect(parse_error(step_document(R"({"action":"type","uiId":"7"})")) ==
                   ScriptErrorCode::InvalidStep,
               "type requires text");
        expect(parse_error(step_document(R"({"action":"scrollIntoView","uiId":"5","dx":0})")) ==
                   ScriptErrorCode::InvalidStep,
               "scrollIntoView rejects offsets");
        expect(parse_error(step_document(R"({"action":"scroll","uiId":"5","dx":0})")) ==
                   ScriptErrorCode::InvalidStep,
               "scroll requires dy");
        expect(parse_error(step_document(R"({"action":"audit","profile":"x","path":"../x"})")) ==
                   ScriptErrorCode::InvalidStep,
               "unsafe artifact path rejected");
        expect(parse_error(step_document(R"({"action":"click","uiId":"1","timeoutMs":0})")) ==
                   ScriptErrorCode::InvalidStep,
               "non-positive timeout rejected");
        expect(parse_error(step_document("5")) == ScriptErrorCode::InvalidStep,
               "non-object step rejected");
    }

    if (failures != 0) {
        std::cerr << "script: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
