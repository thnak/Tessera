// SPDX-License-Identifier: MIT
//
// Behaviors in the design-time preview (ADR-064 item 7, spike #658; behavior-hooks-v1.md section
// 10).
//
// The spec's section 10 says the preview "must create the same registry `Instance` from the IR
// entry and drive it with the session's normalized input". This test proves exactly that, and it is
// the evidence the ADR asks for ("the preview session drives the same Instance, or the spike states
// why it cannot"):
//
//   * a component that calls `usePressFeedback` produces one behavior in the session, named from
//   the
//     registry, attached to the node its `ref` resolved to;
//   * a pointer press reaches the behavior through the session's own `dispatch()`, so the outputs
//     track the scene -- this is what makes a Button look pressed in Studio;
//   * the outputs are the manifest's three, in manifest order, and they default to false;
//   * a component that calls no behavior hook produces no behavior at all, which is what keeps
//   every
//     existing preview unchanged.
//
// Deterministic: no device, no clock, no filesystem.

#include "scene_dynamic_test_support.hpp"
#include "scene_prepare.hpp"
#include "scene_session.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace ui = tessera::ui;

int g_failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_behavior: " << message << "\n";
        ++g_failures;
    }
}

cli::ScenePreviewOptions options_for(std::string_view file) {
    cli::ScenePreviewOptions options;
    options.file = file;
    options.salt = "preview";
    options.width = 320.0f;
    options.height = 240.0f;
    options.dpi_scale = 1.0f;
    return options;
}

// A Button that reads `usePressFeedback` and paints its own state, so the outputs are observable
// through the tree the preview builds rather than only through the session's accessor. The hook
// forms are the ones the analyzer accepts: `useRef<Button>(null)` (the ref's element type must be
// the element it is attached to) and an explicit `{}` options object.
inline constexpr std::string_view kPressFeedback = R"(export function Press(): Node {
  const target = useRef<Button>(null);
  const press = usePressFeedback(target, {});
  return (
    <View>
      <Button ref={target} automationId="7">
        {press.pressed ? "Pressed" : "Idle"}
      </Button>
    </View>
  );
}
)";

// The same, painting the hovered output, which a rebuild can show without losing the pointer.
inline constexpr std::string_view kHoverPaint = R"(export function Hover(): Node {
  const target = useRef<Button>(null);
  const press = usePressFeedback(target, {});
  return (
    <View>
      <Button ref={target} automationId="7">
        {press.hovered ? "Over" : "Away"}
      </Button>
    </View>
  );
}
)";

// A component with no behavior hook: the preview must build it exactly as before.
inline constexpr std::string_view kNoBehavior = R"(export function Plain(): Node {
  return (
    <View>
      <Text>Plain</Text>
    </View>
  );
}
)";

// The session for `source`, or nullptr with the reason printed. The `PreparedScene` must outlive
// the session (the session borrows the IR's strings), so the caller owns it.
std::unique_ptr<cli::PreviewSession> session_for(const cli::PreparedScene& scene) {
    if (scene.root == nullptr || !scene.sheet.has_value() || scene.module.components.empty()) {
        std::cerr << "scene_behavior: prepare produced no usable tree\n";
        ++g_failures;
        return nullptr;
    }
    cli::PreviewSession::Options options;
    options.width = 320.0f;
    options.height = 240.0f;
    options.dpi_scale = 1.0f;
    return std::make_unique<cli::PreviewSession>(scene.module, scene.module.components.back(),
                                                 *scene.sheet, options);
}

// The behavior exists, is named from the registry, and is attached to the ref's node.
void test_behavior_attaches() {
    const cli::PreparedScene scene =
        cli::prepare_scene(kPressFeedback, options_for("ui/Press.tsx"));
    expect(scene.error.empty(), "the press-feedback component prepares: " + scene.error);
    if (!scene.error.empty()) {
        return;
    }
    const auto session = session_for(scene);
    if (!session) {
        return;
    }
    expect(session->built(), "the press-feedback component builds: " + session->build_error());
    expect(session->behavior_count() == 1, "one behavior for one hook call (saw " +
                                               std::to_string(session->behavior_count()) + ")");
    if (session->behavior_count() != 1) {
        return;
    }
    expect(session->behavior_name(0) == "press_feedback",
           "the behavior is named from the registry: " + std::string(session->behavior_name(0)));
    expect(session->behavior_attached(0), "the behavior attached to its ref's node");
    // The manifest's three outputs, in manifest order.
    const std::span<const ui::behavior::OutputValue> outputs = session->behavior_outputs(0);
    expect(outputs.size() == 3,
           "press_feedback reports three outputs (saw " + std::to_string(outputs.size()) + ")");
    if (outputs.size() == 3) {
        expect(!outputs[0].boolean, "pressed defaults to false");
        expect(!outputs[1].boolean, "hovered defaults to false");
        expect(!outputs[2].boolean, "focused defaults to false");
    }
}

// A pointer press reaches the behavior through the session's own dispatch, so the outputs follow
// the scene. This is the claim that makes a Button look pressed in Studio.
void test_press_reaches_the_behavior() {
    const cli::PreparedScene scene =
        cli::prepare_scene(kPressFeedback, options_for("ui/Press.tsx"));
    if (!scene.error.empty()) {
        return;
    }
    const auto session = session_for(scene);
    if (!session || session->behavior_count() != 1) {
        return;
    }
    // The button's box, from the live tree: the press must land inside it. The automation id is the
    // IR's `automationId` (7 in the fixture).
    const ui::Node* target = session->find_by_automation_id(7);
    if (target == nullptr) {
        std::cerr << "scene_behavior: no node with automation id 7\n";
        ++g_failures;
        return;
    }
    const ui::rhi::Rect bounds = target->world_bounds();
    const float x = bounds.x + bounds.width * 0.5f;
    const float y = bounds.y + bounds.height * 0.5f;

    cli::PreviewInput move;
    move.kind = cli::PreviewInput::Kind::PointerMove;
    move.x = x;
    move.y = y;
    session->dispatch(move);

    cli::PreviewInput down;
    down.kind = cli::PreviewInput::Kind::PointerDown;
    down.x = x;
    down.y = y;
    session->dispatch(down);

    const std::span<const ui::behavior::OutputValue> outputs = session->behavior_outputs(0);
    if (outputs.size() == 3) {
        expect(outputs[0].boolean, "pressed after a pointer down inside the target");
        expect(outputs[1].boolean, "hovered after a pointer move onto the target");
    }

    cli::PreviewInput up;
    up.kind = cli::PreviewInput::Kind::PointerUp;
    up.x = x;
    up.y = y;
    session->dispatch(up);
    const std::span<const ui::behavior::OutputValue> after = session->behavior_outputs(0);
    if (after.size() == 3) {
        expect(!after[0].boolean, "not pressed after the pointer is released");
    }
}

bool painted(const cli::PreviewSession& session, std::string_view text) {
    const auto snapshot = tessera::ui::automation::snapshot_scene(session.root());
    return snapshot && scene_dynamic_test::has_text(scene_dynamic_test::texts(*snapshot), text);
}

// A behavior output read in an expression reaches the tree: the live instance feeds the scope, the
// tree is re-evaluated, and the pointer that produced the output survives the rebuild.
void test_output_reaches_the_tree() {
    const cli::PreparedScene scene = cli::prepare_scene(kHoverPaint, options_for("ui/Hover.tsx"));
    if (!scene.error.empty()) {
        expect(false, "the hover component prepares: " + scene.error);
        return;
    }
    const auto session = session_for(scene);
    if (!session || session->behavior_count() != 1) {
        return;
    }
    expect(painted(*session, "Away"), "the default output paints before any input");
    const ui::Node* target = session->find_by_automation_id(7);
    if (target == nullptr) {
        expect(false, "no node with automation id 7");
        return;
    }
    const ui::rhi::Rect bounds = target->world_bounds();
    cli::PreviewInput move;
    move.kind = cli::PreviewInput::Kind::PointerMove;
    move.x = bounds.x + bounds.width * 0.5f;
    move.y = bounds.y + bounds.height * 0.5f;
    session->dispatch(move);
    expect(painted(*session, "Over"), "a pointer over the target paints the hovered output");
    expect(session->behavior_outputs(0).size() == 3 && session->behavior_outputs(0)[1].boolean,
           "the rebuild kept the hover the output came from");
    move.x = 1000.0f;
    move.y = 1000.0f;
    session->dispatch(move);
    expect(painted(*session, "Away"), "leaving the target paints the default again");
}

// A component with no behavior hook produces no behavior, which is what keeps every existing
// preview unchanged.
void test_no_behavior_means_no_instance() {
    const cli::PreparedScene scene = cli::prepare_scene(kNoBehavior, options_for("ui/Plain.tsx"));
    expect(scene.error.empty(), "the plain component prepares: " + scene.error);
    if (!scene.error.empty()) {
        return;
    }
    const auto session = session_for(scene);
    if (!session) {
        return;
    }
    expect(session->built(), "the plain component builds: " + session->build_error());
    expect(session->behavior_count() == 0,
           "a component with no behavior hook has no behavior (saw " +
               std::to_string(session->behavior_count()) + ")");
    expect(session->behavior_outputs(0).empty(), "there is no behavior 0 to read");
    expect(!session->behavior_attached(0), "there is no behavior 0 to be attached");
}

} // namespace

int main() {
    test_behavior_attaches();
    test_press_reaches_the_behavior();
    test_no_behavior_means_no_instance();
    test_output_reaches_the_tree();
    if (g_failures != 0) {
        std::cerr << "scene_behavior: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_behavior: ok\n";
    return 0;
}
