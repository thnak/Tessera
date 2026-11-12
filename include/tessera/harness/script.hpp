// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/command.hpp"
#include "tessera/harness/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::harness {

// v1 script document (docs/specs/headless-automation-v1.md section 4). Parsing is all-or-nothing:
// a script is untrusted input (section 9), so one malformed step rejects the document before the
// first host command runs, instead of half-running a test and reporting a pass.
//
// The caps below bound the parser, not just the runner: a script can never allocate more than
// kMaxScriptBytes of input, kMaxScriptSteps step records, or a capped text/path field. The step cap
// matches the runner's default max_steps so a parsed script is always executable as a whole.
inline constexpr std::size_t kMaxScriptBytes = std::size_t{1} << 20U; // 1 MiB (the line cap)
inline constexpr std::size_t kMaxScriptSteps = 256;
inline constexpr std::size_t kMaxScriptTextBytes = std::size_t{64} << 10U; // 64 KiB
inline constexpr std::size_t kMaxScriptItemKeyBytes = 256;
inline constexpr std::size_t kMaxScriptLocaleBytes = 64;
inline constexpr std::size_t kMaxScriptProfileNameBytes = 64;
inline constexpr std::uint32_t kMaxScriptProfileDimension = 16'384;
inline constexpr double kMaxScriptScale = 8.0;
inline constexpr std::uint64_t kMaxScriptStepTimeoutMs = 60'000;

// The actions v1 can execute through the `CommandHandler` seam. Section 4 also names lifecycle,
// wait, assert, screenshot, custom-element, navigation and overlay actions; those need host
// capabilities that AutomationHost does not expose yet, so the parser rejects them explicitly
// (UnsupportedAction) rather than silently skipping a step.
enum class ScriptAction {
    Click,
    Type, // the section 4 spelling; dispatched as the `setValue` command
    Focus,
    Scroll,
    ScrollIntoView,
    Snapshot,
    Trace,
    Audit,
};

[[nodiscard]] std::string_view to_string(ScriptAction action) noexcept;
[[nodiscard]] std::optional<ScriptAction> script_action_from_string(std::string_view text) noexcept;

// True for every action name in the section 4 list, including the unimplemented ones. The parser
// distinguishes "known but not implemented yet" from "typo" with this.
[[nodiscard]] bool is_spec_script_action(std::string_view text) noexcept;

// Stable failures of the script lane. They are disjoint from ProtocolErrorCode: a script can fail
// before any command is dispatched (MalformedScript, InvalidStep, ...), and a command that failed
// carries its own protocol code in ScriptError::protocol_code.
enum class ScriptErrorCode {
    MalformedScript,    // not JSON/an object, missing required member, unknown top-level member
    UnsupportedVersion, // scriptVersion is not 1
    UnknownAction,      // action is not in the section 4 list at all
    UnsupportedAction,  // action is in the list but v1 cannot execute it
    InvalidStep,        // step is not an object, an unknown/ill-typed/missing step field
    ScriptTooLarge,     // document exceeds kMaxScriptBytes
    StepCapExceeded,    // the step count cap (parse or budget) was hit
    StepTimeout,        // a step exceeded its time budget
    RunTimeout,         // the whole run exceeded its time budget
    Cancelled,          // the caller cancelled the run
    HandlerError,       // the handler returned a ProtocolError
    ArtifactError,      // a snapshot document failed the path/size/write contract
    Internal,           // the harness itself failed (for example result encoding)
};

[[nodiscard]] std::string_view to_string(ScriptErrorCode code) noexcept;

// Sentinel for "not tied to a step" in ScriptError::step and the report.
inline constexpr std::size_t kNoScriptStep = static_cast<std::size_t>(-1);

struct ScriptError {
    ScriptErrorCode code = ScriptErrorCode::MalformedScript;
    std::string message;
    std::string detail;
    std::size_t step = kNoScriptStep;
    // The handler's typed failure, when the failure came from a command. Kept separate so a client
    // can retry on `timeout`/`uiIdNotFound` without parsing the message.
    std::optional<ProtocolErrorCode> protocol_code;

    friend bool operator==(const ScriptError&, const ScriptError&) = default;
};

// Section 4 `profile`. The runner only validates and carries it: applying a size/DPI/locale is a
// host responsibility, and a script is never allowed to redefine the process environment.
struct ScriptProfile {
    std::uint32_t width = 1280;
    std::uint32_t height = 800;
    double scale = 1.0;
    std::string locale = "en-US";

    friend bool operator==(const ScriptProfile&, const ScriptProfile&) = default;
};

struct ScriptStep {
    ScriptAction action = ScriptAction::Click;
    std::optional<UiId> ui_id; // required for click/type/focus/scroll
    std::optional<std::string> item_key;
    std::string text; // type (setValue)
    double scroll_dx = 0.0;
    double scroll_dy = 0.0;
    std::string path;    // snapshot/trace/audit artifact target, empty means "document default"
    std::string profile; // audit profile
    bool include_hidden = false;
    std::uint64_t timeout_us = 0; // 0 = use the runner budget

    friend bool operator==(const ScriptStep&, const ScriptStep&) = default;
};

struct Script {
    std::int64_t version = 1;
    ScriptProfile profile;
    std::string app_config; // section 4 `app.config`, a safe relative path or empty
    std::vector<ScriptStep> steps;

    friend bool operator==(const Script&, const Script&) = default;
};

// Parses one section 4 document. Closed member sets everywhere: an unknown top-level or step
// member is an error, because silently ignoring a misspelled `uiId` would execute a different test
// than the author wrote. UiIds must be decimal strings (section 4 examples; the protocol's small
// integer form is a hand-written-client convenience this untrusted format does not need).
[[nodiscard]] std::expected<Script, ScriptError> parse_script(std::string_view text);

} // namespace tessera::harness
