// SPDX-License-Identifier: MIT

// The studio's foldable preview (issue #408): a simulated `FoldInfo` reaches the SAME metrics and
// safe-area path the native app runs, so a `SafeArea` in the preview resolves exactly as it does on
// a device - and a posture change is applied in ONE layout pass, with no frame of stale padding.
//
// **What "the same path" means here, and how this file proves it rather than asserting it.**
//   * The preview builds a `hal::WindowMetrics` from the posture (`simulated_metrics`, which calls
//     `hal::fold_info_for`).
//   * `layout_scene()` passes that snapshot to `ui::style::apply_safe_area_style()`, which parses
//     the component's class tokens and calls `hal::resolve_safe_area()` - the very function
//     `AppHost::build_layout_input()` calls with the very metrics the OS produced.
//   * The numbers below are therefore checked twice: once against the arithmetic (an inset the
//     test computes from the hinge rectangle), and once against a native `AppHost` run on a
//     hand-built `WindowMetrics` carrying the same fold. If the preview had a second, preview-only
//     resolution, the two would disagree and the last case would fail.
//
// The scene is written with the tokens `packages/tessera-ui/SafeArea.tsx` emits (`tsafe
// safe-edges-... safe-sources-...`), because the preview's `build_scene()` does not splice
// `NodeKind::Component` instances yet (see the note in scene_build.hpp): the *layout* path is what
// this issue adds, and the component lowering is unchanged by it.

#include "scene_layout.hpp"
#include "scene_prepare.hpp"
#include "scene_preview.hpp"
#include "scene_session.hpp"
#include "tessera/app/host.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace hal = tessera::hal;
namespace ui = tessera::ui;
namespace rhi = tessera::rendering::rhi;

int failures = 0;

bool expect(bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "scene_fold: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.5f;
}

// A page whose content sits inside a SafeArea. The content child takes the SafeArea's CONTENT box
// (a flex-growing `100%` box), which is exactly the single pane the hinge leaves: `padding` is
// inside the border box, so the wrapper keeps the window's size and only its content shrinks by the
// resolved inset. That is the observable, and it is the same shape
// `library_safe_area_runtime_tests` asserts against the app host.
//
// **Why the tokens are written out.** `packages/tessera-ui/SafeArea.tsx` lowers to exactly this
// `View` with those class tokens (see tests/golden/codegen/SafeArea.g.hpp); the component only
// declares *what it wants* and the host owns the numbers. The preview's `build_scene()` does not
// splice `NodeKind::Component` instances yet - it reports them as unsupported and the omission is
// recorded in scene_build.hpp and docs/design/compiler-wasm.md 7.1 - so a page that imports
// `<SafeArea>` produces no tree here, while the *same* tokens do. That gap is in the component
// lowering, not in this issue's layout path: what #408 adds is the metrics those tokens resolve
// against, and it is proven below against a real `AppHost` running an identical tree.
constexpr const char* kPage =
    "export function Folded() {\n"
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

cli::ScenePreviewOptions options_for(hal::FoldPosture posture, float width, float height) {
    cli::ScenePreviewOptions options;
    options.file = "ui/Folded.tsx";
    options.salt = "fold-check";
    options.width = width;
    options.height = height;
    options.posture = posture;
    return options;
}

// The `SafeArea` wrapper's CONTENT box, which is what the resolved padding shapes: the wrapper's
// own border box keeps the window's width, and its first child (sized 100%) is the content area.
const ui::Node* content_of(const ui::Node& root) {
    const ui::Node* safe = find_token(root, "tsafe");
    if (safe == nullptr || safe->children().empty()) {
        return nullptr;
    }
    return safe->children().front().get();
}

// --- 1. The metrics the preview lays out against ------------------------------------------------

void test_simulated_metrics_carry_the_fold() {
    // A posture is data, exactly like a size: the snapshot is a pure function of the two.
    cli::SceneMetrics metrics;
    metrics.width = 1000.0f;
    metrics.height = 700.0f;
    metrics.posture = hal::FoldPosture::Book;
    const hal::WindowMetrics snapshot = cli::simulated_metrics(metrics);
    expect(snapshot.logical_width == 1000 && snapshot.logical_height == 700,
           "the simulated snapshot carries the logical extent");
    expect(snapshot.fold.state == hal::FoldState::HalfOpened,
           "the snapshot carries the fold state");
    expect(snapshot.fold.orientation == hal::FoldOrientation::Vertical,
           "book puts a vertical hinge in the snapshot");
    expect(near(snapshot.fold.hinge_bounds.x, 488.0f) &&
               near(snapshot.fold.hinge_bounds.width, 24.0f),
           "the hinge rectangle is in the snapshot, centred on the width");

    // Two runs of one posture agree byte for byte, which is what makes the studio reproducible.
    const hal::WindowMetrics again = cli::simulated_metrics(metrics);
    expect(hal::fold_changed(snapshot.fold, again.fold) == false,
           "two runs of one posture produce the same fold");

    // The no-fold default is the pre-#408 behaviour: it contributes no insets, so no existing
    // preview golden or parity gate can move because of this slice.
    cli::SceneMetrics unfolded;
    unfolded.width = 1000.0f;
    unfolded.height = 700.0f;
    const hal::WindowMetrics plain = cli::simulated_metrics(unfolded);
    expect(plain.fold.state == hal::FoldState::None, "the default has no fold");
    expect(near(plain.fold.hinge_bounds.width, 0.0f), "the default has no hinge rectangle");
}

// --- 2. The hinge reaches the safe-area path ---------------------------------------------------

void test_book_insets_the_single_pane() {
    // Book mode on a 1000x700 window: a 24 px vertical band centred at x=488. A single pane keeps
    // the larger side; the two sides are equal here, and the tie keeps the first (left), so the
    // SafeArea pads on the RIGHT by 1000 - 488 = 512 and the content starts at the window edge.
    cli::PreparedScene scene =
        cli::prepare_scene(kPage, options_for(hal::FoldPosture::Book, 1000.0f, 700.0f), "Folded");
    expect(scene.error.empty(), "the book preview builds: " + scene.error);
    if (!scene.error.empty() || scene.root == nullptr) {
        return;
    }
    const ui::Node* content = content_of(*scene.root);
    expect(content != nullptr, "the SafeArea has a content node");
    if (content != nullptr) {
        expect(near(content->world_bounds().x, 0.0f),
               "book keeps the left pane, so the content is not pushed right");
        expect(near(content->world_bounds().width, 488.0f),
               "book's content area is the left pane (1000 - 512 right inset)");
    }

    // The wrapper's own box is the whole window: `padding` keeps the background under the unsafe
    // area, so only the content moves. That is the difference `mode="margin"` makes.
    const ui::Node* safe = find_token(*scene.root, "tsafe");
    expect(safe != nullptr, "the page has a SafeArea node");
    if (safe != nullptr) {
        expect(near(safe->world_bounds().width, 1000.0f),
               "padding mode keeps the SafeArea's border box at the window width");
    }
}

void test_tabletop_insets_the_single_pane() {
    // Tabletop on the same window: a 24 px horizontal band centred at y=338; the tie keeps the top
    // pane, so the SafeArea is 338 tall and the content inside it starts at y=0.
    cli::PreparedScene scene = cli::prepare_scene(
        kPage, options_for(hal::FoldPosture::Tabletop, 1000.0f, 700.0f), "Folded");
    expect(scene.error.empty(), "the tabletop preview builds: " + scene.error);
    if (!scene.error.empty() || scene.root == nullptr) {
        return;
    }
    const ui::Node* content = content_of(*scene.root);
    expect(content != nullptr, "the SafeArea has a content node");
    if (content == nullptr) {
        return;
    }
    expect(near(content->world_bounds().height, 338.0f),
           "tabletop's content area is the top pane (700 - 362 bottom inset)");
    expect(near(content->world_bounds().width, 1000.0f), "tabletop spans the full width");
}

void test_flat_restores_the_whole_window() {
    // The issue's "a round trip back to `flat` restores the original frame": an unfolded device has
    // no hinge insets at all, so the SafeArea is the whole window again.
    cli::PreparedScene flat =
        cli::prepare_scene(kPage, options_for(hal::FoldPosture::Flat, 1000.0f, 700.0f), "Folded");
    expect(flat.error.empty(), "the flat preview builds: " + flat.error);
    if (flat.error.empty() && flat.root != nullptr) {
        const ui::Node* content = content_of(*flat.root);
        expect(content != nullptr && near(content->world_bounds().width, 1000.0f) &&
                   near(content->world_bounds().height, 700.0f),
               "flat's content area fills the window");
    }

    // And the no-posture default is the same frame, which is what keeps the existing gates stable.
    cli::ScenePreviewOptions none;
    none.file = "ui/Folded.tsx";
    none.width = 1000.0f;
    none.height = 700.0f;
    cli::PreparedScene unfolded = cli::prepare_scene(kPage, none, "Folded");
    expect(unfolded.error.empty(), "the unfolded preview builds: " + unfolded.error);
    if (unfolded.error.empty() && unfolded.root != nullptr) {
        const ui::Node* content = content_of(*unfolded.root);
        expect(content != nullptr && near(content->world_bounds().width, 1000.0f),
               "no posture is the same frame as `flat`");
    }
}

// --- 3. The SAME path as native: the preview against a real AppHost -----------------------------

// One window around a hand-built scene with the SafeArea token set, driven by a real `AppHost`.
// This is the native half: it feeds `set_window_metrics()` the very `WindowMetrics` the preview
// simulated, so the two sides differ only in who produced the snapshot.
//
// It compiles the PREVIEW's own stylesheet (`preview_stylesheet()`), not a hand-written one,
// because a comparison between two different sheets would be measuring the sheets instead of the
// safe-area path. With one sheet and one tree shape, the only remaining variable is the metrics
// source.
struct NativeWindow {
    NativeWindow(float width, float height)
        : loop(clock) {
        root.set_element_kind("View");
        root.set_class_name("fill");
        safe = &root.add_child(std::make_unique<ui::Node>(2));
        safe->set_element_kind("View");
        safe->set_class_name("tsafe safe-edges-top+right+bottom+left "
                             "safe-sources-system-bars+cutout+ime+hinge");
        content = &safe->add_child(std::make_unique<ui::Node>(3));
        content->set_element_kind("View");
        content->set_inline_style("width", "100%");
        content->set_inline_style("height", "100%");
        content->set_inline_style("flex-grow", "1");

        tessera::app::HostConfig config;
        config.width = width;
        config.height = height;
        host = std::make_unique<tessera::app::AppHost>(
            root, tessera::ui::style::compile_stylesheet(cli::preview_stylesheet()), loop, config);
    }

    void frames(int count) {
        for (int i = 0; i < count; ++i) {
            (void)host->begin_frame(1.0 / 60.0);
            host->end_frame();
        }
    }

    tessera::runtime::ManualClock clock;
    ui::Node root{1000};
    tessera::runtime::RuntimeLoop loop;
    std::unique_ptr<tessera::app::AppHost> host;
    ui::Node* safe = nullptr;
    ui::Node* content = nullptr;
};

void test_preview_matches_a_native_host() {
    // For each posture: lay the same page out twice, once in the preview with a simulated fold and
    // once in a real AppHost given the preview's own `WindowMetrics`. The SafeArea boxes must
    // agree. This is the strongest available statement of "no parallel path": the native side is
    // the production frame loop, and it is fed the snapshot the preview produced.
    for (const hal::FoldPosture posture :
         {hal::FoldPosture::Flat, hal::FoldPosture::Book, hal::FoldPosture::Tabletop}) {
        cli::PreparedScene scene =
            cli::prepare_scene(kPage, options_for(posture, 1000.0f, 700.0f), "Folded");
        expect(scene.error.empty(), "the preview builds: " + scene.error);
        if (!scene.error.empty() || scene.root == nullptr) {
            continue;
        }
        const ui::Node* preview_content = content_of(*scene.root);
        expect(preview_content != nullptr, "the preview's SafeArea has a content node");
        if (preview_content == nullptr) {
            continue;
        }

        NativeWindow native(1000.0f, 700.0f);
        // The preview's own snapshot, not a re-derived one: a fold this test invented could agree
        // with a preview that had re-derived it the same wrong way.
        hal::WindowMetrics metrics = scene.metrics;
        metrics.insets = {};
        metrics.ime_insets = {};
        metrics.gesture_insets = {};
        native.host->set_window_metrics(metrics);
        native.frames(1);

        const rhi::Rect want = native.content->world_bounds();
        const rhi::Rect got = preview_content->world_bounds();
        const std::string name{hal::fold_posture_name(posture)};
        expect(near(got.x, want.x) && near(got.y, want.y) && near(got.width, want.width) &&
                   near(got.height, want.height),
               "the preview's content box equals the native host's for posture `" + name +
                   "` (preview " + std::to_string(got.x) + "," + std::to_string(got.y) + " " +
                   std::to_string(got.width) + "x" + std::to_string(got.height) + "; native " +
                   std::to_string(want.x) + "," + std::to_string(want.y) + " " +
                   std::to_string(want.width) + "x" + std::to_string(want.height) + ")");
    }
}

// --- 4. One layout pass per posture change, with no stale padding ------------------------------

void test_posture_change_is_one_pass() {
    // The acceptance forbids "a frame of stale padding". The proof is structural and observable:
    //   1. `PreviewSession::set_posture()` re-lays out INSIDE the call, so by the time it returns
    //      every box already carries the new posture.
    //   2. It reports whether a pass ran, so a caller cannot repaint on a change that did not
    //      happen and a test can count passes.
    //   3. Asking for the posture already in force does no pass at all.
    //
    // The boxes are read immediately after the call with no intervening frame, which is exactly the
    // state a painter would see on its first frame after the change.
    cli::ScenePreviewOptions options = options_for(hal::FoldPosture::Flat, 1000.0f, 700.0f);
    cli::PreparedScene prepared = cli::prepare_scene(kPage, options, "Folded");
    expect(prepared.error.empty(), "the session's page builds: " + prepared.error);
    if (!prepared.error.empty()) {
        return;
    }

    cli::PreviewSession::Options session_options;
    session_options.width = options.width;
    session_options.height = options.height;
    session_options.dpi_scale = options.dpi_scale;
    session_options.posture = hal::FoldPosture::Flat;
    cli::PreviewSession session(prepared.module, prepared.module.components.front(),
                                *prepared.sheet, session_options);
    expect(session.built(), "the session built its tree");

    const ui::Node* flat_content = content_of(session.root());
    expect(flat_content != nullptr, "the session tree has a SafeArea content node");
    if (flat_content == nullptr) {
        return;
    }
    expect(near(flat_content->world_bounds().width, 1000.0f), "the flat session fills the window");
    expect(session.metrics().fold.state == hal::FoldState::Flat, "the session's metrics say flat");

    // Flip to book: ONE call, and the boxes are already the book boxes when it returns.
    expect(session.set_posture(hal::FoldPosture::Book),
           "changing the posture runs exactly one layout pass");
    const ui::Node* book_content = content_of(session.root());
    expect(book_content != nullptr && near(book_content->world_bounds().width, 488.0f),
           "the FIRST observation after the change is already the book pane (no stale frame)");
    expect(session.metrics().fold.orientation == hal::FoldOrientation::Vertical,
           "the session's metrics follow the new posture");

    // Asking again is not a change: no second pass, and the same boxes.
    expect(!session.set_posture(hal::FoldPosture::Book),
           "re-applying the current posture runs no layout pass");
    expect(near(content_of(session.root())->world_bounds().width, 488.0f),
           "the boxes are unchanged by a no-op posture set");

    // ... and back to flat restores the original frame, still in one call.
    expect(session.set_posture(hal::FoldPosture::Flat),
           "changing back to flat runs exactly one layout pass");
    expect(near(content_of(session.root())->world_bounds().width, 1000.0f),
           "the round trip back to flat restores the original frame immediately");

    // Tabletop is a different axis: the height is the pane, not the width.
    expect(session.set_posture(hal::FoldPosture::Tabletop), "tabletop is one layout pass too");
    const ui::Node* tabletop_content = content_of(session.root());
    expect(tabletop_content != nullptr && near(tabletop_content->world_bounds().height, 338.0f) &&
               near(tabletop_content->world_bounds().width, 1000.0f),
           "the first observation after a tabletop change is already the top pane");
}

} // namespace

int main() {
    test_simulated_metrics_carry_the_fold();
    test_book_insets_the_single_pane();
    test_tabletop_insets_the_single_pane();
    test_flat_restores_the_whole_window();
    test_preview_matches_a_native_host();
    test_posture_change_is_one_pass();
    if (failures == 0) {
        std::cout << "scene_fold: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
