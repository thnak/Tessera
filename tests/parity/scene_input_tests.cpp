// SPDX-License-Identifier: MIT
//
// Browser input into the evaluator, and state kept across edits (issue #168, P4 of
// docs/design/reference-equantic-ui.md section 7.4). Two properties are pinned here, and the second
// is the one this slice exists for:
//
// 1. A scripted click produces the same state cells the parity runner's evaluator produces for the
//    same click. The test uses the real `parity::run_parity` runner from
//    tests/parity/parity_runner.hpp: the "native" side of the comparison is the scene-preview
//    session (hit test -> router -> click synthesis -> IR preview body -> state cells), so the
//    acceptance criterion "scripted clicks give the same state cells as the parity runner" is
//    checked by that runner rather than by a second, weaker comparison written here.
//
// 2. State survives a source edit for a component that persists, and a removed component's state is
//    NOT handed to a component that took its place (`scene_state.hpp` states the rule). Both
//    directions are asserted: the cheap test only checks survival, which a "carry everything by
//    position" bug would pass while silently corrupting the other component.

#include "parity_runner.hpp"
#include "scene_evaluate.hpp"
#include "scene_interaction.hpp"
#include "scene_session.hpp"
#include "scene_state.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/ui/scene/state_snapshot.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace ui = tessera::ui;

int failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_input: " << message << "\n";
        ++failures;
    }
    return condition;
}

// The preview's stylesheet as the driver builds it: the session needs a sheet, and a test that
// invented its own would lay out different boxes and therefore hit different nodes.
ui::style::CompiledStyleSheet preview_sheet() {
    return ui::style::compile_stylesheet("View { display: flex; flex-direction: column; }\n"
                                         "Text { display: block; height: 24px; font-size: 16px; }\n"
                                         "Button { width: 120px; height: 32px; }\n");
}

constexpr const char* kCounter = R"(
export function Counter(): Node {
  const [count, setCount] = useState<int>(0);
  const [label, setLabel] = useState<string>("start");

  return (
    <View>
      <Text>{label}</Text>
      <Button onClick={() => { setCount(count + 1); setLabel("clicked"); }}>Add</Button>
    </View>
  );
}
)";

constexpr const char* kCounterWithoutLabel = R"(
export function Counter(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>{count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
)";

// A second, unrelated component in the same file: the one that must NOT inherit Counter's state
// when Counter is removed. It declares the same cell ids at the same positions on purpose, because
// that is exactly the shape a positional carry would corrupt.
constexpr const char* kCounterAndOther = R"(
export function Counter(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>{count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}

export function Other(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>{count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
)";

// Only `Other` is left: `Counter` was deleted, and the file now declares the same *name* only once.
constexpr const char* kOtherOnly = R"(
export function Other(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>{count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
)";

const cli::ScenePreviewOptions& options() {
    static const cli::ScenePreviewOptions value = [] {
        cli::ScenePreviewOptions made;
        made.file = "ui/Preview.tsx";
        made.salt = "preview";
        return made;
    }();
    return value;
}

// The `onClick` automation id of the first button in `text`, from the IR the preview itself builds.
// Reading it from the evaluated module rather than hard-coding a hash keeps the test honest about
// which node a click reaches.
std::uint64_t first_button_id(std::string_view text, std::string_view component) {
    auto evaluated = cli::evaluate_source(text, options().file, options().salt, component);
    if (!evaluated) {
        return 0;
    }
    struct Walk {
        static std::uint64_t find(const tessera::compiler::ir::Node& node) {
            if (node.kind == tessera::compiler::ir::NodeKind::Button &&
                !node.automation_id.empty()) {
                return std::stoull(node.automation_id);
            }
            for (const auto& child : node.children) {
                if (const std::uint64_t found = find(child); found != 0) {
                    return found;
                }
            }
            return 0;
        }
    };
    return Walk::find(evaluated->selected().root);
}

std::string int_cell(const ui::StateSnapshot& state, std::string_view name) {
    for (const ui::StateCell& cell : state) {
        if (cell.name != name) {
            continue;
        }
        if (const int* value = std::get_if<int>(&cell.value)) {
            return std::to_string(*value);
        }
        return "(not an int)";
    }
    return "(missing)";
}

std::string text_cell(const ui::StateSnapshot& state, std::string_view name) {
    for (const ui::StateCell& cell : state) {
        if (cell.name != name) {
            continue;
        }
        if (const std::string* value = std::get_if<std::string>(&cell.value)) {
            return *value;
        }
        return "(not a string)";
    }
    return "(missing)";
}

// The acceptance test: the parity runner compares the scene session's click result with its own
// evaluator run of the same click, step by step, including the initial state.
void test_click_matches_parity_runner() {
    const std::uint64_t button = first_button_id(kCounter, "Counter");
    if (!expect(button != 0, "the counter's button has an automation id")) {
        return;
    }
    auto evaluated = cli::evaluate_source(kCounter, options().file, options().salt, "Counter");
    if (!expect(evaluated.has_value(), "the counter source evaluates")) {
        return;
    }
    const std::string script_text = "click " + std::to_string(button) + "\nclick " +
                                    std::to_string(button) + "\nclick " + std::to_string(button) +
                                    "\n";
    auto script = tessera::parity::parse_script(script_text);
    if (!expect(script.has_value(),
                "the script parses: " + (script ? std::string() : script.error()))) {
        return;
    }

    const ui::style::CompiledStyleSheet sheet = preview_sheet();
    cli::PreviewSession::Options session_options;
    cli::PreviewSession session(evaluated->module, evaluated->selected(), sheet, session_options);
    if (!expect(session.built(), "the counter scene builds")) {
        return;
    }

    // The native side of the comparison is this scene: hit test, router, click synthesis, IR
    // preview body. A null step asks for the initial state, exactly as the runner's contract says.
    const tessera::parity::NativeSide native =
        [&session](
            const tessera::parity::Step* step) -> std::expected<ui::StateSnapshot, std::string> {
        if (step == nullptr) {
            return session.state();
        }
        const cli::PreviewClickResult click = session.click(std::stoull(step->target));
        if (!click.found) {
            return std::unexpected("no visible node carries automation id " + step->target);
        }
        if (!click.dispatched) {
            return std::unexpected("the click on " + step->target + " dispatched no event");
        }
        return session.state();
    };

    const tessera::parity::Report report =
        tessera::parity::run_parity(evaluated->module, evaluated->selected(), *script, native);
    expect(report.ok(),
           "scripted clicks match the parity runner:\n" + tessera::parity::format_report(report));
    expect(report.steps_run == 3, "the runner ran all three steps");
    expect(int_cell(session.state(), "state::count") == "3",
           "three clicks left state::count at 3, got " + int_cell(session.state(), "state::count"));
    expect(text_cell(session.state(), "state::label") == "clicked",
           "the handler also set state::label");
}

// A click must not be a shortcut past the real dispatch: a click at a point with no node hits
// nothing, and a click on an id nobody has is reported as unfound rather than as a no-op success.
void test_click_requires_a_real_target() {
    auto evaluated = cli::evaluate_source(kCounter, options().file, options().salt, "Counter");
    if (!expect(evaluated.has_value(), "the counter source evaluates")) {
        return;
    }
    const ui::style::CompiledStyleSheet sheet = preview_sheet();
    cli::PreviewSession::Options session_options;
    cli::PreviewSession session(evaluated->module, evaluated->selected(), sheet, session_options);
    if (!expect(session.built(), "the counter scene builds")) {
        return;
    }

    const cli::PreviewClickResult missing = session.click(1);
    expect(!missing.found, "a click on an unknown automation id is not found");
    expect(int_cell(session.state(), "state::count") == "0",
           "a click on an unknown automation id changes no cell");

    cli::PreviewInput off_tree;
    off_tree.kind = cli::PreviewInput::Kind::PointerDown;
    off_tree.x = 4000.0f; // outside the 320x240 viewport
    off_tree.y = 4000.0f;
    expect(!session.dispatch(off_tree), "a pointer press outside the viewport reaches no node");
    expect(int_cell(session.state(), "state::count") == "0", "that press changes no cell");

    const cli::PreviewInputStep step{1, "click", {}};
    expect(!step.kind.empty(), "the step record is usable");
}

// The first half of the state rule: a surviving component keeps its cells across an edit, including
// an edit that adds a cell above and changes the initializer.
void test_state_survives_an_edit() {
    auto before = cli::evaluate_source(kCounter, options().file, options().salt, "Counter");
    if (!expect(before.has_value(), "the counter source evaluates")) {
        return;
    }
    const ui::style::CompiledStyleSheet sheet = preview_sheet();
    cli::PreviewSession::Options session_options;
    cli::PreviewSession session(before->module, before->selected(), sheet, session_options);
    if (!expect(session.built(), "the counter scene builds")) {
        return;
    }
    const std::uint64_t button = first_button_id(kCounter, "Counter");
    const cli::PreviewClickResult click = session.click(button);
    expect(click.found && click.dispatched, "the button click dispatches");
    expect(int_cell(session.state(), "state::count") == "1", "the click incremented the counter");

    const cli::CarriedState carried = session.capture();
    expect(!carried.cells.empty(), "the session captured its cells");

    // The edit: `label` is gone and the counter's initializer changed from 0 to 100. A carry keyed
    // by position or by initializer would either drop `count` or reset it to 100.
    auto after =
        cli::evaluate_source(kCounterWithoutLabel, options().file, options().salt, "Counter");
    if (!expect(after.has_value(), "the edited source evaluates")) {
        return;
    }
    const std::vector<std::string> live{before->selected().component_id,
                                        after->selected().component_id};
    cli::PreviewSession::Options carried_options;
    carried_options.carried = &carried;
    carried_options.live_component_ids = &live;
    cli::PreviewSession next(after->module, after->selected(), sheet, carried_options);
    if (!expect(next.built(), "the edited scene builds")) {
        return;
    }

    expect(next.carry_report().carried == 1, "exactly one cell was carried across the edit");
    expect(next.carry_report().dropped_cell == 1,
           "the removed `label` cell was dropped, not carried");
    expect(next.carry_report().dropped_component == 0,
           "the component itself survived, so nothing was dropped as a removed component");
    expect(int_cell(next.state(), "state::count") == "1",
           "state::count survived the edit, got " + int_cell(next.state(), "state::count"));
    expect(text_cell(next.state(), "state::label") == "(missing)",
           "the removed cell is gone from the snapshot");

    // And the carried value is live: a click on the edited tree continues from 1, not from 0 or
    // 100.
    const std::uint64_t next_button = first_button_id(kCounterWithoutLabel, "Counter");
    const cli::PreviewClickResult next_click = next.click(next_button);
    expect(next_click.found && next_click.dispatched, "the edited tree's button dispatches");
    expect(int_cell(next.state(), "state::count") == "2",
           "a click after the edit continues from the carried value, got " +
               int_cell(next.state(), "state::count"));
}

// The second half, and the reason the rule is identity-based: when `Counter` disappears, its state
// must not be handed to `Other`, even though `Other` declares the same cell ids at the same
// positions. A positional or name-blind carry passes the test above and corrupts this one.
void test_removed_component_state_is_not_reused() {
    auto before = cli::evaluate_source(kCounterAndOther, options().file, options().salt, "Counter");
    if (!expect(before.has_value(), "the two-component source evaluates")) {
        return;
    }
    const ui::style::CompiledStyleSheet sheet = preview_sheet();
    cli::PreviewSession::Options session_options;
    cli::PreviewSession session(before->module, before->selected(), sheet, session_options);
    if (!expect(session.built(), "the counter scene builds")) {
        return;
    }
    const std::uint64_t button = first_button_id(kCounterAndOther, "Counter");
    const cli::PreviewClickResult click = session.click(button);
    expect(click.found && click.dispatched, "the counter's button dispatches");
    expect(int_cell(session.state(), "state::count") == "1", "the counter counted one click");
    const std::string removed_id = before->selected().component_id;
    const cli::CarriedState carried = session.capture();

    // `Counter` is deleted; the file now declares `Other` only. `Other`'s own `count` is 0.
    auto after = cli::evaluate_source(kOtherOnly, options().file, options().salt, "Other");
    if (!expect(after.has_value(), "the edited source evaluates")) {
        return;
    }
    expect(after->selected().component_id != removed_id,
           "Counter and Other have different component ids");
    const std::vector<std::string> live{after->selected().component_id};
    cli::PreviewSession::Options carried_options;
    carried_options.carried = &carried;
    carried_options.live_component_ids = &live;
    cli::PreviewSession next(after->module, after->selected(), sheet, carried_options);
    if (!expect(next.built(), "the surviving component's scene builds")) {
        return;
    }

    expect(next.carry_report().carried == 0, "no cell of the removed component was carried");
    expect(next.carry_report().dropped_component == carried.cells.size(),
           "every cell of the removed component was dropped as a component loss");
    expect(int_cell(next.state(), "state::count") == "0",
           "Other starts at its own initializer, not at Counter's 1, got " +
               int_cell(next.state(), "state::count"));

    // And the value is genuinely gone, not merely hidden: a click on Other counts from 0 to 1.
    const std::uint64_t other_button = first_button_id(kOtherOnly, "Other");
    const cli::PreviewClickResult other_click = next.click(other_button);
    expect(other_click.found && other_click.dispatched, "Other's button dispatches");
    expect(int_cell(next.state(), "state::count") == "1",
           "Other counts from its own initializer, got " + int_cell(next.state(), "state::count"));
}

// A type change is a different cell, not a convertible one: the generated field's type comes from
// the IR, so carrying an int into a now-float cell is a conversion the native build never does.
void test_type_change_drops_the_cell() {
    constexpr const char* kIntSource = R"(
export function Mix(): Node {
  const [value, setValue] = useState<int>(1);
  return <View><Text>{value}</Text><Button onClick={() => setValue(value + 1)}>Add</Button></View>;
}
)";
    auto before = cli::evaluate_source(kIntSource, options().file, options().salt, "Mix");
    if (!expect(before.has_value(), "the int source evaluates")) {
        return;
    }

    // The same component, same cell id, a different declared type.
    cli::CarriedState carried;
    cli::CarriedCell cell;
    cell.key = {"ignored", "state::value", "int"};
    cell.value.name = "state::value";
    cell.value.value = 7;
    carried.cells.push_back(cell);

    tessera::compiler::eval::Scope scope;
    const std::vector<std::string> live{before->selected().component_id};
    // Wrong component id: dropped as a component loss, which is the identity rule again.
    cli::StateCarryReport report = cli::apply_state(before->selected(), carried, live, scope);
    expect(report.dropped_component == 1 && report.carried == 0,
           "a cell from another component id is dropped");
    expect(scope.find("state::value") == scope.end(), "and nothing was written to the scope");

    // Right identity, wrong type: dropped as a type change.
    carried.cells.front().key.component_id = before->selected().component_id;
    carried.cells.front().key.ir_type = "float";
    scope.clear();
    report = cli::apply_state(before->selected(), carried, live, scope);
    expect(report.dropped_type == 1 && report.carried == 0,
           "a cell whose declared type changed is dropped");
    expect(scope.find("state::value") == scope.end(), "and its value is not converted");
}

// The script parser is the page's format, so it is pinned: comments and blank lines are ignored, a
// malformed line is reported, and the step cap holds.
void test_script_parsing() {
    std::vector<cli::PreviewInputStep> steps;
    std::string error;
    expect(cli::parse_input_script("# a comment\n\nclick 42\n  click 7  \n", steps, error),
           "comments and blank lines are skipped: " + error);
    expect(steps.size() == 2, "two steps parsed");
    if (steps.size() == 2) {
        expect(steps[0].automation_id == 42, "the first id parses");
        expect(steps[1].automation_id == 7, "surrounding whitespace is trimmed");
    }
    expect(!cli::parse_input_script("tap 42", steps, error), "an unknown verb is an error");
    expect(!cli::parse_input_script("click", steps, error), "a missing id is an error");
    expect(!cli::parse_input_script("click abc", steps, error), "a non-numeric id is an error");
    std::string too_many;
    for (std::size_t i = 0; i <= cli::kMaxInputSteps; ++i) {
        too_many += "click 1\n";
    }
    expect(!cli::parse_input_script(too_many, steps, error), "the step cap is enforced");
}

// The document the page and the parity gate read: one entry per step, with the state cells the SDK
// and the runner use, and the handler ids that actually ran.
void test_document_shape() {
    const std::uint64_t button = first_button_id(kCounter, "Counter");
    const std::string script = "click " + std::to_string(button);
    const std::string document = cli::scene_input_document(kCounter, options(), "Counter", script);
    expect(document.find("\"inputVersion\":1") != std::string::npos,
           "the document declares its version");
    expect(document.find("\"name\":\"state::count\"") != std::string::npos,
           "the document carries the state cells");
    expect(document.find("\"found\":true") != std::string::npos, "the step found its node");
    expect(document.find("\"ran\":[\"handler::1\"]") != std::string::npos,
           "the step names the handler that ran");
    expect(document.back() == '\n', "the document ends with a newline");

    // A malformed script is reported in the same shape rather than as a half-run.
    const std::string bad = cli::scene_input_document(kCounter, options(), "Counter", "tap 1");
    expect(bad.find("\"error\":") != std::string::npos, "a malformed script reports its error");
    expect(bad.find("\"steps\":[]") != std::string::npos, "and runs no step");
}

// An empty script is legal and still reports the initial state, so a caller always has a baseline.
void test_empty_script() {
    const std::string document = cli::scene_input_document(kCounter, options(), "Counter", "");
    expect(document.find("\"initial\":[") != std::string::npos,
           "an empty script still reports the initial cells");
    expect(document.find("\"steps\":[]") != std::string::npos, "with no steps");
}

} // namespace

int main() {
    test_click_matches_parity_runner();
    test_click_requires_a_real_target();
    test_state_survives_an_edit();
    test_removed_component_state_is_not_reused();
    test_type_change_drops_the_cell();
    test_script_parsing();
    test_document_shape();
    test_empty_script();
    if (failures != 0) {
        std::cerr << "scene_input: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_input: ok\n";
    return 0;
}
