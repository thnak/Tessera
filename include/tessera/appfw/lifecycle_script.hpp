// SPDX-License-Identifier: MIT
//
// The synthetic lifecycle script (AF3 acceptance: "a synthetic lifecycle script passes per
// platform").
//
// Why this is not a `harness::ScriptAction`: the harness script format executes host commands
// (`click`, `snapshot`, `trace`), and its document is validated all-or-nothing against a closed
// member set, with lifecycle actions deliberately parsed as `unsupportedAction` because
// `AutomationHost` has no capability to express them (docs/specs/headless-automation-v1.md section
// 4). An app lifecycle runs before a `WindowHost` exists, so a lifecycle script cannot be a harness
// step without inventing a host capability the harness spec does not define.
//
// What it is instead: one JSON document, replayed by one state machine (`appfw::Application`), in
// `tests/fixtures/lifecycle/`, checked by the unit lane on every platform this repository builds.
// The shape is deliberately the harness's: `scriptVersion` (exactly 1), a closed member set, and
// steps that are `{event, expect}` pairs, so a reader who knows the automation format can read it.
// The point of the artifact is that Windows and Linux parse and replay the same bytes.
//
// Nothing here is platform-conditional and nothing reads a wall clock, a thread or an OS callback:
// the script is the whole input, and the expected state comes from ADR-050 section 2's table.

#pragma once

#include "tessera/appfw/lifecycle.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

// Caps mirror the harness's own (docs/specs/headless-automation-v1.md section 4): a script is
// untrusted input even when it lives in the repository.
inline constexpr std::size_t kMaxLifecycleScriptBytes = 64 * 1024;
inline constexpr std::size_t kMaxLifecycleScriptSteps = 256;

enum class LifecycleScriptErrorCode : std::uint8_t {
    MalformedScript,
    UnsupportedVersion,
    UnknownEvent,
    UnknownState,
    InvalidStep,
    ScriptTooLarge,
};

[[nodiscard]] constexpr std::string_view
lifecycle_script_error_name(LifecycleScriptErrorCode code) noexcept {
    switch (code) {
    case LifecycleScriptErrorCode::MalformedScript:
        return "malformedScript";
    case LifecycleScriptErrorCode::UnsupportedVersion:
        return "unsupportedVersion";
    case LifecycleScriptErrorCode::UnknownEvent:
        return "unknownEvent";
    case LifecycleScriptErrorCode::UnknownState:
        return "unknownState";
    case LifecycleScriptErrorCode::InvalidStep:
        return "invalidStep";
    case LifecycleScriptErrorCode::ScriptTooLarge:
        return "scriptTooLarge";
    }
    return "malformedScript";
}

struct LifecycleScriptError {
    LifecycleScriptErrorCode code = LifecycleScriptErrorCode::MalformedScript;
    std::string detail;
    // Index of the step that failed, or `kNoLifecycleStep` for a document-level error.
    std::size_t step = static_cast<std::size_t>(-1);

    friend bool operator==(const LifecycleScriptError&, const LifecycleScriptError&) = default;
};

inline constexpr std::size_t kNoLifecycleStep = static_cast<std::size_t>(-1);

// One step: the event to dispatch and the state ADR-050 section 2 derives from it.
struct LifecycleScriptStep {
    LifecycleEvent event{};
    AppState expected = AppState::Active;

    friend constexpr bool operator==(const LifecycleScriptStep&,
                                     const LifecycleScriptStep&) = default;
};

struct LifecycleScript {
    // The document's `script` name, for the report ("desktop").
    std::string name;
    std::vector<LifecycleScriptStep> steps;
};

// Strict, all-or-nothing decode: a bad step rejects the document before the first transition runs.
// The decoder is deliberately tiny and self-contained (an object with a closed member set, arrays,
// strings without escapes, integers) because layer 1 links no JSON library: the harness's decoder
// is a tool dependency, and ADR-050 section 1 keeps the dependency direction one way.
[[nodiscard]] std::expected<LifecycleScript, LifecycleScriptError>
parse_lifecycle_script(std::string_view text);

// What replaying a script did.
struct LifecycleScriptReport {
    std::size_t steps = 0;
    std::size_t transitions = 0;
    // The index of the first step whose derived state did not match its `expect`, or
    // `kNoLifecycleStep` when every step matched.
    std::size_t failed_step = kNoLifecycleStep;
    // The state the failed step produced (meaningful only when `failed_step` is set).
    AppState observed = AppState::Launching;
};

} // namespace tessera::appfw
