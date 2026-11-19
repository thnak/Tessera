// SPDX-License-Identifier: MIT

// `source.patch` through a real evaluation (issue #171, designer-protocol-v1 section 10.2).
//
// **Why this is its own translation unit.** `scene_canvas_patch_tests.cpp` asserts the builder's
// byte ranges from a fixture, with no tree and no evaluation: that is what makes its assertions
// exact. These two tests assert the other half - that the engine gap the slice closed is actually
// closed, and that the session's own `patch()` reaches the builder with the right facts. They need
// a live `CanvasSceneSession`, which the fixture tests deliberately do not.
//
// The round trip under test is: IR element span -> build walk -> session side table ->
// `node_span()`
// -> `PatchContext` -> edit range. Every assertion checks the range against the source text the
// test handed the session, never against a constant, so an off-by-one anywhere along that path
// fails.

#include "canvas_report.hpp"
#include "scene_canvas_patch.hpp"
#include "scene_canvas_session.hpp"
#include "scene_canvas_test_support.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace ir = tessera::compiler::ir;
namespace ui = tessera::ui;

using cli::PatchDestination;
using cli::PatchRequest;
using cli::TextEdit;

int& failures() {
    static int count = 0;
    return count;
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_canvas_patch_session: " << message << "\n";
        ++failures();
    }
}

// The engine gap this slice closes, asserted end to end: a real evaluation carries the IR span of
// the JSX element onto the scene node, so a commit can name the bytes it edits. Without the side
// table this test cannot pass - the scene node has no span of its own.
void test_span_round_trips_through_a_real_evaluation() {
    constexpr std::string_view kSource = R"tsx(export function Card(): Node {
  return (
    <View className="card">
      <Text>Hello</Text>
    </View>
  );
}
)tsx";
    cli::CanvasSceneSession session;
    expect(session.start(canvas_test::source_for(kSource, "ui/Card.tsx")).empty(),
           "the source starts");
    const ui::Node* card = canvas_test::find_class(*session.root(), "card");
    expect(card != nullptr, "the source has a .card node");
    if (card == nullptr) {
        return;
    }
    const ir::SourceSpan* span = session.node_span(card->automation_id());
    expect(span != nullptr, "the node has an IR source span");
    if (span == nullptr) {
        return;
    }
    expect(span->file == "ui/Card.tsx", "the span names the component file");
    // The span is the element's, so it starts at `<View` and covers the opening tag - the range the
    // patch builder searches for the `style` attribute.
    const std::string_view sliced = kSource.substr(span->offset, span->length);
    expect(sliced.starts_with("<View"),
           "the span starts at the element's `<`, got `" + std::string(sliced.substr(0, 8)) + "`");
    expect(sliced.find("className=\"card\"") != std::string_view::npos,
           "the span covers the element's attributes");
    // A node with no automation id cannot be asked about, and the engine's "no id" 0 is not a node.
    expect(session.node_span(0) == nullptr, "automation id 0 names no node");
    expect(session.node_span(999999) == nullptr, "an unknown automation id names no node");
}

// The same round trip through the session's own `patch()`: the span the side table carries is what
// the builder uses to locate the attribute, so a commit on a real evaluation produces an edit whose
// range slices exactly the `style` attribute out of the source.
void test_session_patch_uses_the_span() {
    constexpr std::string_view kSource = R"tsx(export function Card(): Node {
  return (
    <View className="card" style={{ color: "red" }}>
      <Text>Hello</Text>
    </View>
  );
}
)tsx";
    cli::CanvasSceneSession session;
    expect(session.start(canvas_test::source_for(kSource, "ui/Card.tsx")).empty(),
           "the source starts");
    const ui::Node* card = canvas_test::find_class(*session.root(), "card");
    expect(card != nullptr, "the source has a .card node");
    if (card == nullptr) {
        return;
    }
    PatchRequest request;
    request.automation_id = card->automation_id();
    request.destination = PatchDestination::Inline;
    // The declarations are the node's overrides (issue #536), never the request's: the session
    // commits what `style.override.set` put in the layer.
    expect(session.override_set(card->automation_id(), "padding-left", "8px").ok,
           "the override is accepted");
    const cli::SourcePatchResult result = session.patch(request);
    expect(result.ok, "the session produces a patch, got `" +
                          std::string(cli::patch_error_name(result.reason)) +
                          "`: " + result.detail);
    if (!result.ok) {
        return;
    }
    expect(!result.patch_id.empty(), "the patch carries an id for the host to name");
    expect(result.patch.edits.size() == 1, "the commit is one edit");
    if (result.patch.edits.size() != 1) {
        return;
    }
    const TextEdit& edit = result.patch.edits.front();
    expect(kSource.substr(edit.offset, edit.length) == R"(style={{ color: "red" }})",
           "the edit replaces exactly the style attribute, got `" +
               std::string(kSource.substr(edit.offset, edit.length)) + "`");
    // The report the page parses: one line, `ok:true`, the edit and its base version.
    const std::string report = cli::canvas_patch_report_json(result);
    expect(report.find("\"ok\":true") != std::string::npos, "the report says ok");
    expect(report.find("\"patchId\":\"patch:") != std::string::npos,
           "the report carries the patch id");
    expect(report.find("\"baseVersions\":{\"ui/Card.tsx\":1}") != std::string::npos,
           "the report carries the base version, got `" + report + "`");
    expect(report.back() == '\n', "the report is one line");
}

// A commit on a session with no evaluation is `no_such_node`, not a crash and not an empty patch: a
// page that asks before the first Run must get a well-formed answer.
void test_patch_before_a_run_is_no_such_node() {
    cli::CanvasSceneSession session;
    PatchRequest request;
    request.automation_id = 1;
    request.destination = PatchDestination::Inline;
    const cli::SourcePatchResult result = session.patch(request);
    expect(!result.ok, "a commit before the first Run produces no patch");
    expect(cli::patch_error_name(result.reason) == "no_such_node",
           "a commit before the first Run is `no_such_node`");
    expect(result.patch.edits.empty(), "a failed commit carries no edits");
    expect(result.patch.base_versions.empty(), "a failed commit carries no base versions");
}

} // namespace

int main() {
    test_span_round_trips_through_a_real_evaluation();
    test_session_patch_uses_the_span();
    test_patch_before_a_run_is_no_such_node();
    if (failures() != 0) {
        std::cerr << "scene_canvas_patch_session: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_patch_session: ok\n";
    return 0;
}
