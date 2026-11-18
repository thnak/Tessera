// SPDX-License-Identifier: MIT
//
// The skin's safe area reaching the engine's layout (issue #171's skins slice,
// `designer-studio-v1.md` sections 5.1 and 5.2).
//
// **Why this file exists.** The spec's sentence is "its insets are passed to the viewer as
// `safeAreas` in `viewport.set`, exactly as `WindowMetrics::insets` would arrive from the HAL", and
// section 5.1 adds "the skin must never change the engine's logical size behind its back". A page
// that drew a phone's status bar over a tree laid out as if the bar were not there would satisfy
// neither: the picture would show a device the layout does not know about. So the geometry half of
// the slice's acceptance is asserted here, on the engine side, where it is deterministic:
//
//  1. **Non-zero insets pad the content.** A `SafeArea` node's content box shrinks by exactly the
//     insets the skin declared, through `hal::resolve_safe_area()` - the same function the native
//     app runs, not a preview-only copy.
//  2. **Zero insets leave the frame unchanged.** The desktop answer, and the pre-#171 behaviour, so
//     no existing preview golden or parity gate can move because of this slice.
//  3. **A skin change is one layout pass, with no stale padding.** The boxes are already the new
//     ones when `set_safe_area()` returns, and asking for the safe area already in force runs no
//     pass at all.
//  4. **The cutout rectangle reaches the metrics.** A notch is a rectangle the layout can read, not
//     only a picture the page draws.
//  5. **The ABI refuses a bad skin.** A negative inset is an error rather than a silent zero, which
//     is what makes "a skin that hard-codes a colour is a bug" have a sibling rule for geometry.
//
// The scene is written with the tokens `packages/tessera-ui/SafeArea.tsx` emits (`tsafe
// safe-edges-... safe-sources-...`), for the reason `scene_fold_tests.cpp` gives: the preview's
// `build_scene()` does not splice `NodeKind::Component` instances yet, and the *layout* path is
// what this slice adds.

#include "scene_canvas_session.hpp"
#include "scene_layout.hpp"
#include "wasm_canvas_safe_area.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace hal = tessera::hal;
namespace ui = tessera::ui;

int failures = 0;

bool expect(bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "scene_skin_geometry: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.5f;
}

// A page whose content sits inside a SafeArea, sized to fill whatever the wrapper's content box is.
// The wrapper keeps the window's border box (padding mode), so the observable is the content
// child's box: it shrinks by exactly the resolved insets.
constexpr const char* kPage =
    "export function Skinned() {\n"
    "  return (\n"
    "    <View className=\"fill\">\n"
    "      <View className=\"tsafe safe-edges-top+right+bottom+left "
    "safe-sources-system-bars+cutout+ime+hinge\">\n"
    "        <View style={{ width: \"100%\", height: \"100%\", flexGrow: \"1\" }} />\n"
    "      </View>\n"
    "    </View>\n"
    "  );\n"
    "}\n";

const ui::Node* find_token(const ui::Node& node, std::string_view token) {
    const std::string classes = " " + std::string(node.class_name()) + " ";
    if (classes.find(" " + std::string(token) + " ") != std::string::npos) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_token(*child, token)) {
            return found;
        }
    }
    return nullptr;
}

// The SafeArea wrapper's CONTENT box: the first child, sized 100% of the padded content area.
const ui::Node* content_of(const ui::Node& root) {
    const ui::Node* safe = find_token(root, "tsafe");
    if (safe == nullptr || safe->children().empty()) {
        return nullptr;
    }
    return safe->children().front().get();
}

cli::CanvasSessionSource source_for(float width, float height) {
    cli::CanvasSessionSource source;
    source.text = kPage;
    source.file = "ui/Skinned.tsx";
    source.salt = "skin-check";
    source.width = width;
    source.height = height;
    source.dpi_scale = 1.0f;
    return source;
}

// --- 1 + 2. The metrics carry the skin, and zero insets change nothing -------------------------

void test_simulated_metrics_carry_the_skin() {
    cli::SceneMetrics metrics;
    metrics.width = 412.0f;
    metrics.height = 915.0f;
    metrics.insets = {0.0f, 24.0f, 0.0f, 24.0f};
    metrics.cutouts.bounds[0] = {206.0f, 14.0f, 10.0f, 10.0f};
    metrics.cutouts.count = 1;

    const hal::WindowMetrics snapshot = cli::simulated_metrics(metrics);
    expect(near(snapshot.insets.top, 24.0f) && near(snapshot.insets.bottom, 24.0f),
           "the simulated snapshot carries the skin's insets");
    expect(near(snapshot.insets.left, 0.0f) && near(snapshot.insets.right, 0.0f),
           "the sides the skin left at zero stay zero");
    expect(snapshot.cutouts.count == 1, "the snapshot carries the skin's cutout");
    expect(near(snapshot.cutouts.bounds[0].x, 206.0f) &&
               near(snapshot.cutouts.bounds[0].width, 10.0f),
           "the cutout rectangle is in the snapshot");

    // The desktop answer, and the pre-#171 behaviour: a skin with no insets produces the same
    // snapshot the preview produced before this slice, so no existing golden can move.
    cli::SceneMetrics plain;
    plain.width = 412.0f;
    plain.height = 915.0f;
    const hal::WindowMetrics bare = cli::simulated_metrics(plain);
    expect(near(bare.insets.top, 0.0f) && near(bare.insets.bottom, 0.0f) &&
               near(bare.insets.left, 0.0f) && near(bare.insets.right, 0.0f),
           "a skin with no insets contributes no padding");
    expect(bare.cutouts.count == 0, "a skin with no cutout contributes no rectangle");

    // Two runs of one skin agree, which is what makes the studio reproducible.
    const hal::WindowMetrics again = cli::simulated_metrics(metrics);
    expect(cli::same_insets(again.insets, snapshot.insets) &&
               cli::same_cutouts(again.cutouts, snapshot.cutouts),
           "two runs of one skin produce the same safe area");
}

// --- 3. The insets reach the safe-area path, and zero leaves the frame unchanged ----------------

void test_insets_pad_the_content() {
    // A 412x915 phone with a 24 px status bar and a 24 px navigation bar: the content box is the
    // window minus those two, and the sides are untouched.
    cli::CanvasSessionSource source = source_for(412.0f, 915.0f);
    source.insets = {0.0f, 24.0f, 0.0f, 24.0f};
    cli::CanvasSceneSession session;
    expect(session.start(source).empty(), "the skinned source starts");
    const ui::Node* content = content_of(*session.root());
    expect(content != nullptr, "the SafeArea has a content node");
    if (content == nullptr) {
        return;
    }
    expect(near(content->world_bounds().y, 24.0f), "the content starts below the skin's top inset");
    expect(near(content->world_bounds().height, 915.0f - 48.0f),
           "the content is the window minus both bars");
    expect(near(content->world_bounds().width, 412.0f), "a zero side inset leaves the width alone");
    expect(near(session.metrics().insets.top, 24.0f),
           "the session's metrics report the skin it laid out against");

    // The same page with no insets is the pre-#171 frame: the content fills the window.
    cli::CanvasSceneSession plain;
    expect(plain.start(source_for(412.0f, 915.0f)).empty(), "the unskinned source starts");
    const ui::Node* full = content_of(*plain.root());
    expect(full != nullptr && near(full->world_bounds().y, 0.0f) &&
               near(full->world_bounds().height, 915.0f),
           "zero insets leave the content filling the window");
}

// --- 4. A skin change is one layout pass, with no stale padding --------------------------------

void test_skin_change_is_one_pass() {
    cli::CanvasSessionSource source = source_for(412.0f, 915.0f);
    cli::CanvasSceneSession session;
    expect(session.start(source).empty(), "the session starts with no insets");
    const ui::Node* before = content_of(*session.root());
    expect(before != nullptr && near(before->world_bounds().y, 0.0f),
           "the first frame has no padding");

    // The skin picker moves to a phone: ONE call, and the boxes are already the phone's when it
    // returns. This is the "no frame of stale padding" rule, and it is why the insets are a layout
    // input rather than a page-only overlay.
    expect(session.set_safe_area({0.0f, 24.0f, 0.0f, 24.0f}, {}),
           "changing the safe area runs exactly one layout pass");
    const ui::Node* after = content_of(*session.root());
    expect(after != nullptr && near(after->world_bounds().y, 24.0f),
           "the FIRST observation after the change is already padded (no stale frame)");
    expect(near(session.metrics().insets.top, 24.0f), "the session's metrics follow the new skin");

    // Asking again is not a change: no second pass, and the same boxes.
    expect(!session.set_safe_area({0.0f, 24.0f, 0.0f, 24.0f}, {}),
           "re-applying the current safe area runs no layout pass");
    expect(near(content_of(*session.root())->world_bounds().y, 24.0f),
           "the boxes are unchanged by a no-op safe-area set");

    // ... and back to the desktop frame restores the original boxes, still in one call.
    expect(session.set_safe_area({}, {}), "clearing the safe area runs exactly one layout pass");
    expect(near(content_of(*session.root())->world_bounds().y, 0.0f),
           "the round trip back to no insets restores the original frame immediately");

    // A cutout alone is a change too: the rectangle is a layout input even when the insets are
    // already zero, because a `SafeArea` with `safe-sources-cutout` reads it.
    hal::CutoutInfo notch;
    notch.bounds[0] = {186.0f, 0.0f, 40.0f, 30.0f};
    notch.count = 1;
    expect(session.set_safe_area({}, notch), "adding a cutout runs one layout pass");
    expect(session.metrics().cutouts.count == 1, "the session's metrics carry the cutout");
    expect(!session.set_safe_area({}, notch), "re-applying the same cutout runs no pass");
}

// --- 5. The ABI refuses a bad skin -------------------------------------------------------------

void test_the_abi_refuses_a_bad_skin() {
    // A negative inset is a manifest bug, not a padding of zero: the page must hear about it.
    const cli::CanvasSafeAreaRequest negative =
        cli::canvas_safe_area_request(0.0f, -8.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    expect(!negative.error.empty(), "a negative inset is refused");
    expect(negative.error.find("TESSERA_STUDIO_SKIN") == 0,
           "the refusal carries the spec's TESSERA_STUDIO_SKIN code");

    // A zero rectangle is "no cutout", not a notch in the corner.
    const cli::CanvasSafeAreaRequest none =
        cli::canvas_safe_area_request(0.0f, 24.0f, 0.0f, 24.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    expect(none.error.empty(), "a zero cutout is accepted");
    expect(none.cutouts.count == 0, "a zero cutout is dropped rather than kept as a rectangle");
    expect(near(none.insets.top, 24.0f), "the insets survive a zero cutout");

    // A real cutout is kept, with its rectangle.
    const cli::CanvasSafeAreaRequest notch =
        cli::canvas_safe_area_request(0.0f, 24.0f, 0.0f, 24.0f, 206.0f, 14.0f, 10.0f, 10.0f);
    expect(notch.error.empty(), "a real cutout is accepted");
    expect(notch.cutouts.count == 1, "the cutout is kept");
    expect(near(notch.cutouts.bounds[0].x, 206.0f) && near(notch.cutouts.bounds[0].y, 14.0f),
           "the cutout rectangle is where the skin put it");
}

} // namespace

int main() {
    test_simulated_metrics_carry_the_skin();
    test_insets_pad_the_content();
    test_skin_change_is_one_pass();
    test_the_abi_refuses_a_bad_skin();
    if (failures == 0) {
        std::cout << "scene_skin_geometry: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
