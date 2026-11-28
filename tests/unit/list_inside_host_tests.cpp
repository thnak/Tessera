// SPDX-License-Identifier: MIT
//
// `list-style-position: inside` through the app host (docs/design/list-style-inside.md): the first
// Text of an item gives up the marker's box on its first line, so it can need more lines than with
// an outside marker, and flipping the position reflows it (the layout-unchanged gate must not
// answer "unchanged" for a change of only list-style-position).

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace paint = tessera::ui::paint;
using tessera::ui::Node;

int failures = 0;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "list_inside_host: " << message << "\n";
        ++failures;
    }
    return condition;
}

struct Recorder final : paint::PaintSink {
    void fill_rect(const tessera::rendering::rhi::Rect&,
                   const tessera::rendering::rhi::Color&) override {}
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&, float) override {}
    void draw_text(const paint::TextDraw& run) override {
        runs.emplace_back(std::string(run.text), run.rect.x);
    }
    std::vector<std::pair<std::string, float>> runs;
};

constexpr const char* kSheet = R"(
.item { width: 120px; list-style-type: decimal; }
.in { list-style-position: inside; }
.out { list-style-position: outside; }
)";

// Six four-letter words: 8 px per stub glyph, so 120 px holds three of them per line outside (two
// lines), while inside the first line holds two beside the 24 px "1. " box (three lines).
constexpr const char* kWords = "aaaa bbbb cccc dddd eeee ffff";

struct Scene {
    std::unique_ptr<Node> root = std::make_unique<Node>(1);
    Node* item = nullptr;
    Node* text = nullptr;

    Scene() {
        root->set_bounds({0, 0, 400, 300});
        auto li = std::make_unique<Node>(2);
        li->set_role("listitem");
        li->set_class_name("item in");
        auto leaf = std::make_unique<Node>(3);
        leaf->set_element_kind("Text");
        tessera::ui::PaintData data;
        data.kind = tessera::ui::PaintKind::Text;
        data.text = kWords;
        data.wrap = tessera::ui::PaintWrap::Wrap;
        leaf->set_paint_data(std::move(data));
        text = leaf.get();
        li->add_child(std::move(leaf));
        item = li.get();
        root->add_child(std::move(li));
    }
};

float frame_height(tessera::app::AppHost& host, const Node& node, Recorder& sink) {
    sink.runs.clear();
    (void)host.begin_frame(1.0 / 60.0);
    (void)host.paint(sink);
    host.end_frame();
    return node.bounds().height;
}

} // namespace

int main() {
    Scene scene;
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 400.0f;
    config.height = 300.0f;
    tessera::app::AppHost host(*scene.root, tessera::ui::style::compile_stylesheet(kSheet), loop,
                               config);
    Recorder sink;

    const float inside = frame_height(host, *scene.text, sink);
    bool marker_before_text = false;
    for (std::size_t i = 0; i + 1 < sink.runs.size(); ++i) {
        marker_before_text = marker_before_text || (sink.runs[i].first == "1. " &&
                                                    sink.runs[i + 1].first == "aaaa bbbb" &&
                                                    sink.runs[i + 1].second > sink.runs[i].second);
    }
    expect(marker_before_text, "the marker is drawn, and the first line starts after it");

    scene.item->set_class_name("item out");
    const float outside = frame_height(host, *scene.text, sink);
    expect(inside > outside, "the box costs a line: inside is taller than outside (" +
                                 std::to_string(inside) + " vs " + std::to_string(outside) + ")");

    scene.item->set_class_name("item in");
    const float again = frame_height(host, *scene.text, sink);
    expect(again == inside, "flipping back reflows to the inside height");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
