// SPDX-License-Identifier: MIT
//
// Conditional and repeated children in the design-time preview (issue #389).
//
// The acceptance sentence is about what a page paints, so this test drives the path the page drives
// -- `prepare_scene()` and `PreviewSession` -- and reads the automation snapshot, the artifact the
// parity gate compares. It pins the claims docs/design/preview-dynamic-children.md makes:
//
//   * a conditional child paints the branch its state selects, chosen by the condition the
//   evaluator
//     computes from the same scope a click writes: one notion of state drives text, branches and
//     lists, and there is no second evaluator;
//   * changing that state swaps the painted branch -- "a conditional re-evaluates when its state
//   cell
//     changes" -- driven through a real click, not a private shortcut;
//   * a repeated child paints one subtree per item of the list its `source` resolves to, in list
//   order,
//     with the numeric item key published on the item root;
//   * the bounds are reported, never swallowed: a nested template fails with a named diagnostic,
//   and a
//     **hidden** branch the preview cannot build does not fail the tree that is shown -- while the
//     same subtree reported once it is the shown one.
//
// Deterministic, no device, no clock, no filesystem: the same source and state produce the same
// tree.

#include "scene_dynamic_test_support.hpp"
#include "scene_session.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace automation = tessera::ui::automation;
namespace test = scene_dynamic_test;

int g_failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_dynamic: " << message << "\n";
        ++g_failures;
    }
}

void test_conditional_selects_a_branch() {
    cli::PreparedScene scene =
        cli::prepare_scene(test::kConditional, test::options_for("ui/Toggle.tsx"));
    expect(scene.error.empty(), "a conditional child prepares: " + scene.error);
    if (scene.root == nullptr) {
        return;
    }
    std::string error;
    const automation::UiSnapshot snapshot = test::snapshot_of(scene, error);
    expect(error.empty(), "the conditional scene walks: " + error);

    const std::vector<std::string> painted = test::texts(snapshot);
    expect(test::has_text(painted, "Count: 0"),
           "the initial state prints into its template: " + test::join(painted));
    // `count` is 0: `count > 0` is false, so the else branch paints and the then branch is absent
    // -- absent because its condition is false, not because the preview refused it.
    expect(test::has_text(painted, "Empty"),
           "the branch whose condition holds paints: " + test::join(painted));
    expect(!test::has_text(painted, "Positive"),
           "the branch whose condition fails is absent: " + test::join(painted));
}

// The state that drives the condition is the state a click writes: a real gesture through the
// session, then a rebuild from the same scope in which the other branch appears. Nothing here sets
// the scope -- `dispatch()` and the handler body do.
void test_conditional_reevaluates_after_a_click() {
    cli::PreparedScene scene =
        cli::prepare_scene(test::kConditional, test::options_for("ui/Toggle.tsx"));
    expect(scene.error.empty(), "the clickable conditional prepares: " + scene.error);
    if (!scene.error.empty() || scene.root == nullptr || !scene.sheet.has_value() ||
        scene.module.components.empty()) {
        return;
    }
    cli::PreviewSession::Options options;
    options.width = 320.0f;
    options.height = 240.0f;
    options.dpi_scale = 1.0f;
    cli::PreviewSession session(scene.module, scene.module.components.back(), *scene.sheet,
                                options);
    expect(session.built(), "the session builds the conditional tree");

    // The button is found by the automation id the snapshot reports, so the test uses the identity
    // a page uses rather than a private node pointer.
    const auto before = automation::snapshot_scene(session.root());
    expect(before.has_value(), "the session's tree walks");
    if (!before) {
        return;
    }
    std::uint64_t button_id = 0;
    for (const automation::UiSnapshotNode& node : before->nodes) {
        if (node.kind == "Button") {
            button_id = node.ui_id;
            break;
        }
    }
    expect(button_id != 0, "the conditional source has an addressable button");
    if (button_id == 0) {
        return;
    }
    expect(test::has_text(test::texts(*before), "Empty"),
           "the first evaluation paints the false branch: " + test::join(test::texts(*before)));

    // One click runs `setCount(count + 1)` through the evaluator, which writes the same scope the
    // condition reads; the session then rebuilds the tree from it.
    const cli::PreviewClickResult clicked = session.click(button_id);
    expect(clicked.found, "the button is found by its automation id");
    expect(!clicked.ran.empty(), "the click ran the handler body");

    const auto after = automation::snapshot_scene(session.root());
    expect(after.has_value(), "the rebuilt tree walks");
    if (!after) {
        return;
    }
    const std::vector<std::string> painted = test::texts(*after);
    // Both halves matter: the branch that became true is painted, and the one that became false is
    // gone. Only the second half would also hold for "the preview painted nothing".
    expect(test::has_text(painted, "Positive"),
           "the branch the new state selects paints: " + test::join(painted));
    expect(!test::has_text(painted, "Empty"),
           "the branch the new state rejects is gone: " + test::join(painted));
    expect(test::has_text(painted, "Count: 1"),
           "the template re-evaluates too: " + test::join(painted));
}

void test_repeated_child() {
    cli::PreparedScene scene =
        cli::prepare_scene(test::kRepeated, test::options_for("ui/Rows.tsx"));
    expect(scene.error.empty(), "a repeated child prepares: " + scene.error);
    if (scene.root == nullptr) {
        return;
    }
    std::string error;
    const automation::UiSnapshot snapshot = test::snapshot_of(scene, error);
    expect(error.empty(), "the repeated scene walks: " + error);

    const std::vector<std::string> painted = test::texts(snapshot);
    expect(test::has_text(painted, "alpha"), "the first item paints: " + test::join(painted));
    expect(test::has_text(painted, "beta"), "the second item paints: " + test::join(painted));
    expect(test::has_text(painted, "gamma"), "the third item paints: " + test::join(painted));
    // One subtree per item and no more: the count is what catches "painted the template text once".
    expect(painted.size() == 3, "exactly one text node per item: " + test::join(painted));
    // List order, which is the order `apply_list_<n>()` inserts items in.
    expect(painted.size() == 3 && painted[0] == "alpha" && painted[1] == "beta" &&
               painted[2] == "gamma",
           "the items paint in list order: " + test::join(painted));
}

// The item root carries the item's numeric automation key, which is how the automation layer
// addresses exactly one live item (`(template uiId, itemKey)`, automation-ids-v1 section 1).
void test_numeric_item_keys() {
    cli::PreparedScene scene =
        cli::prepare_scene(test::kRepeated, test::options_for("ui/Rows.tsx"));
    if (!scene.error.empty() || scene.root == nullptr) {
        expect(false, "the item-key scene prepares: " + scene.error);
        return;
    }
    std::string error;
    const automation::UiSnapshot snapshot = test::snapshot_of(scene, error);
    expect(error.empty(), "the item-key scene walks: " + error);
    std::vector<std::string> keys;
    for (const automation::UiSnapshotNode& node : snapshot.nodes) {
        if (!node.item_key.empty()) {
            keys.push_back(node.item_key);
        }
    }
    // Decimal, in list order, one per item: `snapshot_scene()` writes the canonical decimal form.
    expect(keys.size() == 3, "one numeric key per item: " + test::join(keys));
    expect(keys.size() == 3 && keys[0] == "0" && keys[1] == "1" && keys[2] == "2",
           "the item keys are the item indices in decimal: " + test::join(keys));
}

void test_empty_list() {
    cli::PreparedScene scene =
        cli::prepare_scene(test::kEmptyList, test::options_for("ui/Empty.tsx"));
    expect(scene.error.empty(), "an empty list prepares: " + scene.error);
    if (scene.root == nullptr) {
        return;
    }
    std::string error;
    const automation::UiSnapshot snapshot = test::snapshot_of(scene, error);
    expect(error.empty(), "the empty-list scene walks: " + error);
    const std::vector<std::string> painted = test::texts(snapshot);
    expect(painted.size() == 1, "an empty list paints no item: " + test::join(painted));
    // The sibling after the empty template still builds: an empty list is not a failed build.
    expect(test::has_text(painted, "After"),
           "the sibling after an empty list paints: " + test::join(painted));
}

void test_sibling_after_list() {
    cli::PreparedScene scene =
        cli::prepare_scene(test::kListThenSibling, test::options_for("ui/One.tsx"));
    expect(scene.error.empty(), "a one-item list with a sibling prepares: " + scene.error);
    if (scene.root == nullptr) {
        return;
    }
    std::string error;
    const automation::UiSnapshot snapshot = test::snapshot_of(scene, error);
    const std::vector<std::string> painted = test::texts(snapshot);
    expect(test::has_text(painted, "only"), "the single item paints: " + test::join(painted));
    expect(test::has_text(painted, "After"),
           "the sibling after the list paints: " + test::join(painted));
}

// A hidden branch whose subtree the preview cannot build must not fail the shown tree: a branch is
// built when its condition holds, so a hidden branch is not a subtree the build has to get through.
// The complement below shows the same subtree *does* report once it is the shown one, which is what
// stops this pair from passing for a preview that builds no branch at all.
void test_hidden_branch_does_not_fail() {
    const cli::PreparedScene scene =
        cli::prepare_scene(test::kHiddenBranch, test::options_for("ui/Hidden.tsx"));
    expect(scene.error.empty(),
           "a hidden branch the preview cannot build does not fail the tree: " + scene.error);
    if (scene.root == nullptr) {
        return;
    }
    std::string error;
    const automation::UiSnapshot snapshot = test::snapshot_of(scene, error);
    const std::vector<std::string> painted = test::texts(snapshot);
    expect(test::has_text(painted, "Base"),
           "the visible part of the tree still paints: " + test::join(painted));

    // The same subtree, shown: the refusal is reported instead of silently painting an empty
    // branch.
    const cli::PreparedScene shown =
        cli::prepare_scene(test::kShownBranch, test::options_for("ui/Shown.tsx"));
    expect(!shown.error.empty(),
           "the same subtree reports once it is the shown branch: " + shown.error);
    expect(shown.error.find("TESSERA_SCENE_UNSUPPORTED") != std::string::npos,
           "the shown-branch refusal is the preview's own named diagnostic: " + shown.error);
    expect(shown.error.find("nested list template") != std::string::npos,
           "the shown-branch refusal names what it cannot build: " + shown.error);
}

// The same source and state produce the same tree: a preview that read a clock or a hash order
// would fail here rather than in a page far from the cause.
void test_determinism() {
    const cli::PreparedScene first =
        cli::prepare_scene(test::kRepeated, test::options_for("ui/Rows.tsx"));
    const cli::PreparedScene second =
        cli::prepare_scene(test::kRepeated, test::options_for("ui/Rows.tsx"));
    if (!first.error.empty() || !second.error.empty() || first.root == nullptr ||
        second.root == nullptr) {
        expect(false, "both evaluations of the repeated source prepare");
        return;
    }
    const auto a = automation::snapshot_scene(*first.root);
    const auto b = automation::snapshot_scene(*second.root);
    expect(a.has_value() && b.has_value(), "both repeated scenes walk");
    if (a && b) {
        expect(*a == *b, "two evaluations produce the same snapshot");
    }
}

} // namespace

int main() {
    test_conditional_selects_a_branch();
    test_conditional_reevaluates_after_a_click();
    test_repeated_child();
    test_numeric_item_keys();
    test_empty_list();
    test_sibling_after_list();
    test_hidden_branch_does_not_fail();
    test_determinism();
    if (g_failures != 0) {
        std::cerr << "scene_dynamic: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_dynamic: all checks passed\n";
    return 0;
}
