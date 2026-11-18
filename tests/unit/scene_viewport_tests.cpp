// SPDX-License-Identifier: MIT
//
// A resize is a layout pass, not a re-run (issue #436): the window or the emulator's device preset
// must re-resolve `@media` and re-lay the RUNNING tree out in place, and a size that crosses no
// breakpoint must not rebuild the cascade at all. Deterministic (null RHI, no browser), this file
// proves the four parts of the acceptance that can be:
//
//  1. `@media (max-width: 600px)` styles and lays out differently at 800 and 500 wide, on the same
//     running tree (and through the Scene view's document path, the same media context).
//  2. A resize that crosses no breakpoint leaves `cascade_generation()` alone while the boxes move.
//  3. Resizing carries the state cells, the hover and the focus the router holds.
//  4. The in-place frame equals a FRESH evaluation at the same size, by snapshot digest - what
//  makes
//     "no re-evaluation" meaningful rather than merely true.
//
// Registered in `src/compiler/codegen/CMakeLists.txt` rather than `tests/unit/CMakeLists.txt`: the
// preview's tests live with the preview module (as `scene_fold` and `scene_canvas_session` do), so
// this lane does not edit the shared registry while other lanes work in it.

#include "scene_canvas_test_support.hpp"
#include "scene_prepare.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/automation/scene_snapshot.hpp"
#include "tessera/ui/automation/snapshot_json.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace style = tessera::ui::style;
namespace ui = tessera::ui;

int failures = 0;

bool expect(bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "scene_viewport: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.5f;
}

// The fixture the issue asks for: one page carrying a state cell, a control with a `:hover` rule,
// and a background a width breakpoint changes. `width: 100%` also makes the LAYOUT consequence
// visible (the page is the viewport). **Two buttons, and the second one is why:** a click that
// changes state REBUILDS the tree, and a rebuild resets the router (its hover/active/focus pointers
// point into the tree being replaced), so a state-changing click cannot leave a hover behind. The
// second button has no handler: pressing it focuses it and changes nothing.
constexpr std::string_view kPage = R"(import "./Preview.css";
export function Page(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View className="page">
      <Text>Title</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
      <Button>Stop</Button>
    </View>
  );
}
)";

constexpr std::string_view kCss = R"(
.page { width: 100%; height: 100%; background-color: rgb(20, 40, 60); }
Button { width: 120px; height: 32px; background-color: rgb(45, 108, 223); }
Button:hover { background-color: rgb(61, 125, 239); }
@media (max-width: 600px) {
  .page { background-color: rgb(200, 30, 30); }
}
)";

// What the sheet declares: an assertion compares the cascade's answer with the author's text.
constexpr std::string_view kWidePage = "rgb(20, 40, 60)";
constexpr std::string_view kNarrowPage = "rgb(200, 30, 30)";
constexpr std::string_view kButtonHover = "rgb(61, 125, 239)";

constexpr double kFrame = 1.0 / 60.0;

cli::CanvasSessionSource source_at(float width, float height) {
    cli::CanvasSessionSource source;
    source.text = kPage;
    source.file = "ui/Page.tsx";
    source.salt = "preview";
    source.stylesheet = kCss;
    source.width = width;
    source.height = height;
    source.dpi_scale = 1.0f;
    return source;
}

cli::ScenePreviewOptions options_at(float width, float height) {
    cli::ScenePreviewOptions options;
    options.file = "ui/Page.tsx";
    options.salt = "preview";
    options.stylesheet = kCss;
    options.width = width;
    options.height = height;
    options.dpi_scale = 1.0f;
    return options;
}

const ui::Node* find_class(const ui::Node& node, std::string_view name) {
    const std::string classes = " " + std::string(node.class_name()) + " ";
    if (classes.find(" " + std::string(name) + " ") != std::string::npos) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_class(*child, name)) {
            return found;
        }
    }
    return nullptr;
}

// The computed `background-color` through the real cascade: if this string moves, a rule matched.
std::string background_from(const style::ComputedStyle& computed) {
    const std::string* value = computed.find("background-color");
    return value != nullptr ? *value : std::string();
}

std::string background_of(const style::CompiledStyleSheet& sheet, const ui::Node& node) {
    style::SceneStyleTree tree(node);
    return background_from(tree.compute(sheet, node));
}

std::string background_of(const cli::CanvasSceneSession& session, const ui::Node& node) {
    return background_of(session.sheet(), node);
}

std::string background_of(tessera::ui::paint::StyleProvider& provider, const ui::Node& node) {
    return background_from(provider.compute(node));
}

// The canonical scene snapshot of `root`, hashed: `snapshot_to_json` is the artifact the native
// harness and the parity gates serialize, so this compares the tree the engine would report.
std::string digest_of(const ui::Node& root) {
    ui::automation::SceneSnapshotOptions options;
    options.snapshot.sequence = 0;
    const auto snapshot = ui::automation::snapshot_scene(root, options);
    if (!snapshot.has_value()) {
        return "no-snapshot";
    }
    const std::string json = ui::automation::snapshot_to_json(*snapshot);
    char text[32];
    std::snprintf(text, sizeof text, "%016llx",
                  static_cast<unsigned long long>(style::fnv1a64(json)));
    return std::string(text);
}

// --- 1. The media context reaches the sheet, and the running tree re-styles in place
// --------------

void test_breakpoints_reach_the_preview_document() {
    // The Scene view's document path: one source and sheet, two rectangles, two answers. Before
    // #436 the sheet resolved against `MediaContext`'s default 1280x720 whatever the size.
    const cli::PreparedScene wide = cli::prepare_scene(kPage, options_at(800.0f, 600.0f), "Page");
    const cli::PreparedScene narrow = cli::prepare_scene(kPage, options_at(500.0f, 600.0f), "Page");
    expect(wide.error.empty(), "the 800-wide preview builds: " + wide.error);
    expect(narrow.error.empty(), "the 500-wide preview builds: " + narrow.error);
    if (wide.root == nullptr || narrow.root == nullptr || !wide.sheet.has_value() ||
        !narrow.sheet.has_value()) {
        return;
    }
    const ui::Node* wide_page = find_class(*wide.root, "page");
    const ui::Node* narrow_page = find_class(*narrow.root, "page");
    expect(wide_page != nullptr && narrow_page != nullptr, "both documents have the page node");
    if (wide_page == nullptr || narrow_page == nullptr) {
        return;
    }
    expect(background_of(*wide.sheet, *wide_page) == kWidePage,
           "at 800 wide the base rule applies (" + background_of(*wide.sheet, *wide_page) + ")");
    expect(background_of(*narrow.sheet, *narrow_page) == kNarrowPage,
           "at 500 wide the breakpoint rule applies (" +
               background_of(*narrow.sheet, *narrow_page) + ")");
    expect(near(narrow_page->world_bounds().width, 500.0f),
           "the narrow document's page is the narrow viewport");
}

void test_set_viewport_styles_the_running_tree() {
    cli::CanvasSceneSession session;
    expect(session.start(source_at(800.0f, 600.0f)).empty(), "the page starts at 800x600");
    const ui::Node* page = find_class(*session.root(), "page");
    if (page == nullptr) {
        expect(false, "the running tree has a page node");
        return;
    }
    expect(background_of(session, *page) == kWidePage, "the running tree starts wide");
    expect(session.cascade_generation() == 1, "one start is one cascade generation");

    // One call: same tree, same state scope, a new rectangle. The box AND the style move.
    expect(session.set_viewport(500.0f, 600.0f, 1.0f),
           "crossing the breakpoint runs a layout pass");
    expect(session.cascade_generation() == 2,
           "crossing the breakpoint re-resolves the cascade exactly once");
    expect(background_of(session, *page) == kNarrowPage,
           "the running tree now resolves the narrow rule (" + background_of(session, *page) + ")");
    expect(near(page->world_bounds().width, 500.0f), "and it is laid out at 500 wide");
    expect(session.needs_frame(), "a resize owes exactly one frame");

    // Asking for the rectangle already in force is not a change: no pass, and no frame owed.
    canvas_test::CanvasHarness canvas;
    if (canvas.ready()) {
        expect(canvas.draw(session), "the resize's frame is painted");
        expect(!session.set_viewport(500.0f, 600.0f, 1.0f),
               "a resize to the current viewport runs no pass");
        expect(!session.needs_frame(), "and owes no frame");
        expect(session.cascade_generation() == 2, "and does not rebuild anything");
    }
}

void test_no_crossing_resize_does_not_rebuild_the_cascade() {
    cli::CanvasSceneSession session;
    expect(session.start(source_at(800.0f, 600.0f)).empty(), "the page starts at 800x600");
    canvas_test::CanvasHarness canvas;
    if (!canvas.ready()) {
        expect(false, "the null backend creates the frame's target");
        return;
    }
    expect(canvas.draw(session), "the Run's frame is painted");
    const std::uint64_t created = session.cascade_generation();
    const ui::Node* page = find_class(*session.root(), "page");
    if (page == nullptr) {
        expect(false, "the running tree has a page node");
        return;
    }

    // 800 -> 700 -> 900: the page stays on the same side of the 600 px breakpoint, so the cascade
    // cannot move while the BOXES do - which is what makes the counter the evidence.
    for (const float width : {700.0f, 900.0f}) {
        expect(session.set_viewport(width, 600.0f, 1.0f),
               "a resize inside one breakpoint still runs its layout pass");
        expect(session.cascade_generation() == created,
               "a resize that crosses no breakpoint does not rebuild the cascade");
        expect(near(page->world_bounds().width, width),
               "the boxes followed the resize (no stale layout)");
        expect(background_of(session, *page) == kWidePage, "and the wide rule still applies");
        expect(canvas.draw(session), "each resize owes exactly one frame");
    }
    // Cross it, and exactly one rebuild happens; come back, and exactly one more.
    expect(session.set_viewport(500.0f, 600.0f, 1.0f), "crossing down runs a pass");
    expect(session.cascade_generation() == created + 1, "crossing down rebuilds once");
    expect(background_of(session, *page) == kNarrowPage, "the narrow rule applies");
    expect(canvas.draw(session), "the crossing's frame is painted");
    expect(session.set_viewport(900.0f, 600.0f, 1.0f), "crossing back runs a pass");
    expect(session.cascade_generation() == created + 2, "crossing back rebuilds once");
    expect(background_of(session, *page) == kWidePage, "and the wide rule is back");
}

// --- 2. State, hover and focus survive the resize ----------------------------------------------

void test_resize_keeps_state_hover_and_focus() {
    cli::CanvasSceneSession session;
    expect(session.start(source_at(800.0f, 600.0f)).empty(), "the page starts at 800x600");
    canvas_test::CanvasHarness canvas;
    if (!canvas.ready()) {
        expect(false, "the null backend creates the frame's target");
        return;
    }
    expect(canvas.draw(session), "the Run's frame is painted");

    // Two buttons: `Add` changes state, `Stop` does not. `find_all` is preorder, so the second one
    // is the quiet button.
    const std::vector<const ui::Node*> buttons = canvas_test::find_all(*session.root(), "Button");
    expect(buttons.size() == 2, "the page has two buttons");
    if (buttons.size() != 2) {
        return;
    }
    const ui::Node* add = buttons[0];

    // A state-changing click. It rebuilds the tree (which resets the router), so it is done FIRST.
    const canvas_test::Point add_centre = canvas_test::centre_of(*add);
    (void)session.pointer(cli::PreviewInput::Kind::PointerMove, add_centre.x, add_centre.y);
    (void)session.pointer(cli::PreviewInput::Kind::PointerDown, add_centre.x, add_centre.y);
    (void)session.pointer(cli::PreviewInput::Kind::PointerUp, add_centre.x, add_centre.y);
    expect(canvas_test::counter_value(session) == 1, "the click incremented the counter");
    expect(canvas.draw(session), "the click's frame is painted");

    // Now establish a hover AND a focus that no state change will clear: move onto the quiet button
    // and press it. Its bounds are unchanged by the rebuild (same tree shape, same text).
    const ui::Node* quiet = canvas_test::find_all(*session.root(), "Button")[1];
    const canvas_test::Point quiet_centre = canvas_test::centre_of(*quiet);
    (void)session.pointer(cli::PreviewInput::Kind::PointerMove, quiet_centre.x, quiet_centre.y);
    (void)session.pointer(cli::PreviewInput::Kind::PointerDown, quiet_centre.x, quiet_centre.y);
    (void)session.pointer(cli::PreviewInput::Kind::PointerUp, quiet_centre.x, quiet_centre.y);
    expect(canvas_test::counter_value(session) == 1,
           "the quiet button's press changed no state (so nothing rebuilt)");
    expect(background_of(session, *quiet) == kButtonHover, "the pointer hovers the quiet button");
    expect(canvas.draw(session), "the press's frame is painted");

    const std::uint64_t hovered =
        session.router()->hovered() != nullptr ? session.router()->hovered()->automation_id() : 0;
    const ui::NodeId focused = session.router()->focus_manager().focused() != nullptr
                                   ? session.router()->focus_manager().focused()->id()
                                   : 0;
    expect(hovered != 0, "the router recorded a hover target");
    expect(focused != 0, "the press focused the button");

    expect(session.set_viewport(500.0f, 600.0f, 1.0f), "the resize runs one layout pass");
    // The three things a re-evaluation would have destroyed.
    expect(canvas_test::counter_value(session) == 1,
           "the state cell survives the resize (no re-evaluation)");
    expect(session.router()->hovered() != nullptr &&
               session.router()->hovered()->automation_id() == hovered,
           "the hover survives the resize");
    expect(session.router()->focus_manager().focused() != nullptr &&
               session.router()->focus_manager().focused()->id() == focused,
           "the focus survives the resize");
    expect(background_of(session, *quiet) == kButtonHover,
           "the hover still resolves through the cascade after the resize");
    expect(canvas.draw(session), "the resize's frame is painted");
    expect(!session.needs_frame(), "a painted resize owes nothing more");
}

// --- 3. The in-place frame equals a cold evaluation at the same size ---------------------------

void test_set_viewport_equals_a_cold_evaluation() {
    cli::CanvasSceneSession warm;
    expect(warm.start(source_at(800.0f, 600.0f)).empty(), "the warm session starts at 800x600");
    const std::string wide = digest_of(*warm.root());
    expect(warm.set_viewport(500.0f, 600.0f, 1.0f), "the warm session resizes to 500");
    const std::string in_place = digest_of(*warm.root());

    cli::CanvasSceneSession cold;
    expect(cold.start(source_at(500.0f, 600.0f)).empty(), "the cold session starts at 500x600");
    const std::string fresh = digest_of(*cold.root());

    // The most valuable assertion of the issue: in-place and cold agree on the WHOLE tree.
    expect(in_place == fresh, "the in-place frame equals a fresh evaluation at the same size (" +
                                  in_place + " vs " + fresh + ")");
    expect(wide != in_place,
           "and the two sizes are different frames (" + wide + " vs " + in_place + ")");
    std::cout << "scene_viewport: digests wide=" << wide << " in-place=" << in_place
              << " fresh=" << fresh << "\n";

    // The same claim through the session's own sheet: the resized session's cascade was resolved
    // against the narrow viewport, exactly as the cold session's was. Two sessions that started
    // from different sizes therefore end holding the same resolved sheet.
    expect(warm.sheet().media().viewport_width == 500.0,
           "the resized session's cascade was resolved against the new viewport");
    expect(warm.sheet().media().viewport_width == cold.sheet().media().viewport_width,
           "and it is the viewport the cold session resolved against");
}

// --- 4. Native `AppHost` and the preview agree on the same fixture -----------------------------

// One hand-built equivalent of the preview's page, driven by a real `AppHost` with the SAME
// prepared sheet: the only variable left is who produced the media context.
struct NativePage {
    NativePage(float width, float height)
        : loop(clock) {
        root.set_element_kind("View");
        page = &root.add_child(std::make_unique<ui::Node>(2));
        page->set_element_kind("View");
        page->set_class_name("page");
        auto text = std::make_unique<ui::Node>(3);
        text->set_element_kind("Text");
        page->add_child(std::move(text));
        auto button = std::make_unique<ui::Node>(4);
        button->set_element_kind("Button");
        page->add_child(std::move(button));

        tessera::app::HostConfig config;
        config.width = width;
        config.height = height;
        // The media-independent form, which is what lets a later resize re-resolve `@media`; a
        // pre-compiled sheet would freeze the first answer (the caveat `set_appearance` shares).
        host = std::make_unique<tessera::app::AppHost>(root, style::prepare_stylesheet(kCss), loop,
                                                       config);
    }

    void frame() {
        (void)host->begin_frame(kFrame);
        host->end_frame();
    }

    tessera::runtime::ManualClock clock;
    ui::Node root{1};
    tessera::runtime::RuntimeLoop loop;
    std::unique_ptr<tessera::app::AppHost> host;
    ui::Node* page = nullptr;
};

void test_native_and_preview_agree() {
    for (const float width : {800.0f, 500.0f}) {
        const cli::PreparedScene scene =
            cli::prepare_scene(kPage, options_at(width, 600.0f), "Page");
        expect(scene.error.empty(), "the preview builds at " + std::to_string(width));
        if (scene.root == nullptr || !scene.sheet.has_value()) {
            continue;
        }
        const ui::Node* preview_page = find_class(*scene.root, "page");
        expect(preview_page != nullptr, "the preview's page node exists");
        if (preview_page == nullptr) {
            continue;
        }
        NativePage native(width, 600.0f);
        native.frame();
        const std::string want = background_of(*scene.sheet, *preview_page);
        const std::string got = background_of(native.host->styles(), *native.page);
        expect(got == want, "the preview and a real AppHost resolve the same rule at " +
                                std::to_string(width) + " wide (preview " + want + ", native " +
                                got + ")");
        expect(near(preview_page->world_bounds().width, native.page->world_bounds().width),
               "and the same box for the page at " + std::to_string(width) + " wide (preview " +
                   std::to_string(preview_page->world_bounds().width) + ", native " +
                   std::to_string(native.page->world_bounds().width) + ")");
    }
}

// A real window resize, with the generation counter as the evidence (issue #436's native half).
void test_native_resize_reresolves_only_on_a_crossing() {
    NativePage native(800.0f, 600.0f);
    native.frame();
    const std::uint64_t created = native.host->cascade_generation();
    expect(created == 1, "a host resolves its cascade once at construction");
    expect(background_of(native.host->styles(), *native.page) == kWidePage,
           "the native page starts wide");

    native.host->resize(700.0f, 600.0f, 1.0f);
    native.frame();
    expect(native.host->cascade_generation() == created,
           "a native resize that crosses no breakpoint does not rebuild the cascade");
    expect(background_of(native.host->styles(), *native.page) == kWidePage,
           "and the wide rule still applies");
    expect(near(native.page->world_bounds().width, 700.0f), "the box followed the resize");

    native.host->resize(500.0f, 600.0f, 1.0f);
    native.frame();
    expect(native.host->cascade_generation() == created + 1,
           "a native resize that crosses the breakpoint rebuilds the cascade exactly once");
    expect(background_of(native.host->styles(), *native.page) == kNarrowPage,
           "and the narrow rule applies");
    expect(near(native.page->world_bounds().width, 500.0f), "the box followed the breakpoint");

    native.host->resize(900.0f, 600.0f, 1.0f);
    native.frame();
    expect(native.host->cascade_generation() == created + 2, "crossing back rebuilds once more");
    expect(background_of(native.host->styles(), *native.page) == kWidePage,
           "and the wide rule is back");
}

// The other entry that can move the window (issue #436): a metrics-only driver.
void test_native_metrics_move_the_media_context() {
    NativePage native(800.0f, 600.0f);
    native.frame();
    const std::uint64_t created = native.host->cascade_generation();
    expect(native.host->media_context().viewport_width == 800.0,
           "the host's media context is the window it was built for");

    tessera::hal::WindowMetrics metrics;
    metrics.logical_width = 500;
    metrics.logical_height = 600;
    native.host->set_window_metrics(metrics);
    native.frame();
    expect(native.host->media_context().viewport_width == 500.0,
           "a metrics snapshot moves the media context");
    expect(native.host->cascade_generation() == created + 1,
           "and crossing a breakpoint there re-resolves the cascade too");
    expect(background_of(native.host->styles(), *native.page) == kNarrowPage,
           "the narrow rule now applies natively");

    // A snapshot with no usable extent is not a zero-sized window: it moves nothing.
    tessera::hal::WindowMetrics insets_only;
    insets_only.insets = {0.0f, 24.0f, 0.0f, 0.0f};
    native.host->set_window_metrics(insets_only);
    native.frame();
    expect(native.host->media_context().viewport_width == 500.0,
           "an insets-only snapshot does not resize the window");
}

} // namespace

int main() {
    test_breakpoints_reach_the_preview_document();
    test_set_viewport_styles_the_running_tree();
    test_no_crossing_resize_does_not_rebuild_the_cascade();
    test_resize_keeps_state_hover_and_focus();
    test_set_viewport_equals_a_cold_evaluation();
    test_native_and_preview_agree();
    test_native_resize_reresolves_only_on_a_crossing();
    test_native_metrics_move_the_media_context();
    if (failures != 0) {
        std::cerr << "scene_viewport: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_viewport: ok\n";
    return 0;
}
