// SPDX-License-Identifier: MIT

#pragma once

#include "scene_pending.hpp"
#include "scene_platform.hpp"
#include "scene_preview.hpp"
#include "scene_project.hpp"
#include "scene_session.hpp"
#include "scene_state.hpp"
#include "tessera/ui/scene/state_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Browser input into the in-memory evaluator (issue #168, P4 of
// docs/design/reference-equantic-ui.md section 7.4). It is the interaction sibling of
// `scene_preview_document()`: the same evaluation and the same scene, with a script of clicks
// applied through `ui::InputRouter` and the state cells read back after every step.
//
// The result is one JSON document. It exists so the parity gate has an artifact to compare byte for
// byte (scripts/ci/scene_input_parity.py) and so the page can show what a click did without a
// second serializer:
//
//   {"inputVersion":1,
//    "steps":[{"step":0,"kind":"click","automationId":"123","found":true,"dispatched":true,
//              "ran":["handler::1"],"unrun":[],
//              "state":[{"name":"state::count","kind":"int","value":"1"}, ...]}],
//    "carry":{"carried":1,"droppedComponent":0,"droppedCell":0,"droppedType":0,"added":0}}
//
// State cells use the existing `ui::StateCell` representation (`ui/scene/state_snapshot.hpp`): the
// same list a generated `snapshot_state()` returns and `parity::run_parity` compares. `kind` is the
// scalar's C++ kind and `value` its text, so a diff over two documents names the cell that moved.
namespace tessera::compiler::codegen::cli {

// One step of an input script. `click` names the automation id of a node; the session finds it and
// performs the gesture. A step list is bounded by kMaxInputSteps so an untrusted page cannot make
// the evaluator run an unbounded script.
//
// A `platform` step (issue #446) delivers one normalized hardware or environment event through
// `PlatformEventDriver`, the code the canvas session's `platform.event` runs; its reply is read
// from `InputRunResult::platforms` at the same index.
struct PreviewInputStep {
    std::uint64_t automation_id = 0;
    std::string_view kind = "click"; // "click", "platform" or "resource"
    PlatformEvent platform;
    // A `resource` step (issue #657): one control of a fake resource, or a read (`status`).
    PendingControl resource;
    bool resource_status = false;
};

inline constexpr std::size_t kMaxInputSteps = 64;

// A script in the page's own text form, one step per line:
//   click 1234567890123456789
//   platform <type> [value] [@<timestampNs>]   e.g. `platform back`, `platform orientation
//   landscape`
//   resource <fetch|resolve|fail|status> [<name> [value...] [@<epoch>]]   a fake resource's control
//   (`tessera_canvas_resource`'s vocabulary; the reply is the pending report)
// Blank lines and lines starting with `#` are ignored, matching `parity::parse_script`'s shape so a
// reviewer reads one script format in this repo, not two.
[[nodiscard]] std::string_view input_script_format() noexcept;

// Parses `text` into steps. A malformed line is reported rather than skipped: half-running a script
// would compare a different set of steps than the author wrote.
[[nodiscard]] bool parse_input_script(std::string_view text, std::vector<PreviewInputStep>& out,
                                      std::string& error);

// Runs `steps` against the scene for `text` and returns the result document. `options.file`/`salt`
// must match the snapshot driver's so the ids in both documents are the same numbers.
[[nodiscard]] std::string scene_input_document(std::string_view text,
                                               const ScenePreviewOptions& options,
                                               std::string_view component, std::string_view script);
// The same run over a whole project; `options.file` and the stylesheet fields are not read.
[[nodiscard]] std::string scene_input_document(const ProjectInput& project,
                                               const ScenePreviewOptions& options,
                                               std::string_view component, std::string_view script);

// The same run, exposed as values so a test can assert on cells without parsing JSON: the state
// after each step, in step order (index 0 is the state before any step).
struct InputRunResult {
    bool built = false;
    std::string error; // why the source produced no scene
    std::vector<ui::StateSnapshot> states;
    // Parallel to the steps: `clicks[i]` is meaningful for a click step, `platforms[i]` for a
    // platform step; the other one is default-constructed.
    std::vector<PreviewClickResult> clicks;
    std::vector<PlatformReply> platforms;
    // `resources[i]` is the pending report line of a resource step, empty for the others.
    std::vector<std::string> resources;
    std::vector<std::string_view> kinds;
};

[[nodiscard]] InputRunResult run_input_script(std::string_view text,
                                              const ScenePreviewOptions& options,
                                              std::string_view component, std::string_view script);
// The same run over a whole project; `options.file` and the stylesheet fields are not read.
[[nodiscard]] InputRunResult run_input_script(const ProjectInput& project,
                                              const ScenePreviewOptions& options,
                                              std::string_view component, std::string_view script);

// One state snapshot as the canonical cell JSON of the document above (`[]` for no cells). Shared
// with the parity gate so both sides of a comparison use one serializer.
[[nodiscard]] std::string state_cells_json(const ui::StateSnapshot& state);

} // namespace tessera::compiler::codegen::cli
