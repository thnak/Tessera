// SPDX-License-Identifier: MIT
//
// The preview half of ADR-074: the Studio's simulated fold answers `@media (device-posture)` and
// the viewport-segment features through the same `FoldInfo` the layout uses and the same
// `with_fold` the app host calls. A posture change re-resolves the running tree's cascade in place
// only when an answer flips, and a document built at a posture starts with its rules applied.

#include "scene_canvas_session.hpp"
#include "scene_layout.hpp"
#include "scene_prepare.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/scene_style.hpp"
#include "tessera/ui/style/window_environment.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace hal = tessera::hal;
namespace ui = tessera::ui;
namespace style = tessera::ui::style;

int failures = 0;

bool expect(bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "scene_environment: " << message << "\n";
        ++failures;
    }
    return condition;
}

constexpr std::string_view kPage = R"(import "./Preview.css";
export function Page(): Node {
  return (
    <View className="page">
      <Text>Title</Text>
    </View>
  );
}
)";

constexpr std::string_view kCss = R"(
.page {
    width: 100%;
    height: 100%;
    padding-top: env(safe-area-inset-top, 1px);
    margin-left: env(viewport-segment-width 1 0, 7px);
}
)";

cli::CanvasSessionSource source_at(hal::FoldPosture posture) {
    cli::CanvasSessionSource source;
    source.text = kPage;
    source.file = "ui/Page.tsx";
    source.salt = "preview";
    source.stylesheet = kCss;
    source.width = 800.0f;
    source.height = 600.0f;
    source.dpi_scale = 1.0f;
    source.posture = posture;
    return source;
}

const ui::Node* find_page(const ui::Node& node) {
    if (node.class_name() == "page") {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_page(*child)) {
            return found;
        }
    }
    return nullptr;
}

std::string property_of(const cli::CanvasSceneSession& session, const ui::Node& node,
                        const char* name) {
    style::SceneStyleTree tree(node);
    const style::ComputedStyle computed = tree.compute(session.sheet(), node);
    const std::string* value = computed.find(name);
    return value != nullptr ? *value : std::string();
}

hal::WindowInsets top_inset(float top, float bottom = 0.0f) {
    hal::WindowInsets insets;
    insets.top = top;
    insets.bottom = bottom;
    return insets;
}

// The preview and the host build the environment from the same function, so a previewed device and
// a real one cannot answer `env()` differently.
void test_preview_environment_is_the_hosts_mapping() {
    cli::SceneMetrics metrics;
    metrics.width = 1000.0f;
    metrics.height = 700.0f;
    metrics.posture = hal::FoldPosture::Book;
    metrics.insets = top_inset(24.0f);
    const style::StyleEnvironment preview = cli::preview_environment(metrics);
    const hal::WindowMetrics window = cli::simulated_metrics(metrics);
    const style::StyleEnvironment hosted = style::window_environment(window, 1000.0f, 700.0f);
    expect(preview == hosted, "the preview environment is the host's mapping");
    const std::string* top = preview.find("safe-area-inset-top");
    expect(top != nullptr && *top == "24px", "the skin's inset is safe-area-inset-top");
    expect(preview.find("viewport-segment-width 1 0") != nullptr,
           "a book fold has a second segment");
    expect(preview.find("preferred-text-scale") == nullptr,
           "a simulated device has no OS text scale, so the variable does not exist");
}

void test_start_on_a_device() {
    cli::CanvasSceneSession session;
    cli::CanvasSessionSource device = source_at(hal::FoldPosture::Book);
    device.insets = top_inset(40.0f);
    expect(session.start(device).empty(), "the page starts");
    const ui::Node* page = session.root() != nullptr ? find_page(*session.root()) : nullptr;
    if (page == nullptr) {
        expect(false, "the running tree has a page node");
        return;
    }
    expect(property_of(session, *page, "padding-top") == "40px",
           "the first build has the safe-area value");
    expect(property_of(session, *page, "margin-left") != "7px",
           "and the segment that a book fold creates");
}

void test_changes_re_resolve_only_what_a_sheet_reads() {
    cli::CanvasSceneSession session;
    expect(session.start(source_at(hal::FoldPosture::None)).empty(), "the page starts");
    const ui::Node* page = session.root() != nullptr ? find_page(*session.root()) : nullptr;
    if (page == nullptr) {
        expect(false, "the running tree has a page node");
        return;
    }
    expect(property_of(session, *page, "padding-top") == "0px", "no inset resolves to 0px");
    expect(property_of(session, *page, "margin-left") == "7px",
           "no fold: the second segment does not exist, so its fallback applies");
    const std::uint64_t started = session.cascade_generation();

    expect(session.set_safe_area(top_inset(40.0f), {}), "a new inset runs a layout pass");
    expect(session.cascade_generation() == started + 1, "a read variable re-resolves once");
    expect(property_of(session, *page, "padding-top") == "40px", "and the value follows");

    expect(session.set_safe_area(top_inset(40.0f, 34.0f), {}),
           "the bottom inset still re-lays out");
    expect(session.cascade_generation() == started + 1,
           "but a variable the sheet does not read re-resolves nothing");

    expect(session.set_posture(hal::FoldPosture::Book), "folding runs a layout pass");
    expect(session.cascade_generation() == started + 2,
           "a fold that creates a read segment resolves");
    expect(property_of(session, *page, "margin-left") != "7px", "the segment exists now");

    expect(session.set_posture(hal::FoldPosture::Flat), "unfolding runs a layout pass");
    expect(property_of(session, *page, "margin-left") == "7px", "and the fallback is back");
}

} // namespace

int main() {
    test_preview_environment_is_the_hosts_mapping();
    test_start_on_a_device();
    test_changes_re_resolve_only_what_a_sheet_reads();
    if (failures == 0) {
        std::cout << "scene_environment: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
