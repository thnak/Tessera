// SPDX-License-Identifier: MIT
//
// Deterministic builder for the CPU benchmark's 12 000-node reference scene (issue #241). The
// virtual list uses the production `VirtualRange` seam, so only the realized window is in the
// scene: the collection's own unit tests cover the window math; here it is the tree-size evidence
// that a 10 000-row data set costs 25 item nodes.

#include "benchmark_scene.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/virtual/virtual_data_source.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::benchmark {
namespace {

using ui::Node;
namespace virt = tessera::ui::virtualization;
namespace style = tessera::ui::style;

// The fixtures bundle ships one PNG fixture; every image node shares it, so the batched renderer
// uploads one texture and the image draws batch together.
constexpr std::string_view kImageAsset = "images/checker.png";
constexpr float kVirtualItemExtent = 24.0f;
constexpr float kVirtualViewportExtent = 600.0f;

// A fixed 20-rule dashboard stylesheet. Class selectors only, no fonts, no transforms and no
// container queries: the benchmark measures the same style/layout/paint path a real sheet takes,
// but with inputs that are identical on every run.
constexpr std::string_view kStylesheet = R"(
.app-root { background-color: #0b1220; }
.topbar { height: 56px; padding: 8px; background-color: #111827; flex-direction: row;
          align-items: center; gap: 8px; }
.logo { width: 32px; height: 32px; }
.brand { font-size: 18px; color: #f8fafc; margin-right: 16px; }
.nav { flex-direction: row; gap: 8px; flex-grow: 1; }
.nav-item { padding: 6px; background-color: #1e293b; border-radius: 6px; }
.nav-label { font-size: 13px; color: #cbd5e1; }
.body { flex-direction: row; flex-grow: 1; }
.sidebar { width: 220px; padding: 8px; background-color: #0f172a; overflow-y: hidden; }
.menu-row { flex-direction: row; height: 28px; align-items: center; }
.menu-label { font-size: 13px; flex-grow: 1; color: #cbd5e1; }
.menu-badge { font-size: 11px; color: #94a3b8; }
.content { flex-direction: column; flex-grow: 1; padding: 12px; gap: 8px; }
.toolbar { flex-direction: row; height: 36px; gap: 6px; }
.button { padding: 4px; background-color: #2563eb; border-radius: 4px; }
.button-label { font-size: 12px; color: #f8fafc; }
.cards { flex-direction: row; flex-wrap: wrap; gap: 12px; }
.card { width: 220px; height: 168px; padding: 10px; background-color: #1e293b; border-radius: 8px;
        flex-direction: column; gap: 4px; }
.card-image { width: 200px; height: 64px; }
.card-title { font-size: 14px; color: #f1f5f9; }
.card-body { font-size: 12px; color: #94a3b8; }
.card-footer { flex-direction: row; height: 20px; gap: 8px; }
.card-meta { font-size: 11px; color: #64748b; }
.records { flex-direction: column; }
.records-row { flex-direction: row; height: 20px; }
.records-cell { font-size: 11px; flex-grow: 1; color: #cbd5e1; }
.virtual-list { height: 600px; background-color: #0f172a; overflow-y: hidden;
                flex-direction: column; }
.virtual-item { height: 24px; flex-direction: row; gap: 6px; }
.item-title { font-size: 12px; flex-grow: 1; color: #e2e8f0; }
.item-sub { font-size: 11px; color: #94a3b8; }
.item-meta { font-size: 11px; color: #64748b; }
.item-badge { font-size: 10px; color: #38bdf8; }
)";

[[nodiscard]] std::string numbered(std::string_view prefix, std::uint64_t number) {
    std::string result(prefix);
    result += std::to_string(number);
    return result;
}

} // namespace

// One synthetic data set: 100 000 stable keys, fixed 24 px rows. The range realizes the window the
// 600 px scrollport covers, which is all the scene contains.
struct SceneBuilder::VirtualSource final : virt::VirtualDataSource {
    explicit VirtualSource(SceneBuilder& owner)
        : owner_(owner) {}

    [[nodiscard]] std::size_t item_count() const override {
        return 100000;
    }
    [[nodiscard]] virt::VirtualItemId item_key(std::size_t index) const override {
        return virt::VirtualItemId{index + 1};
    }
    [[nodiscard]] virt::VirtualItemMeasure
    measure(std::size_t, const virt::MeasureConstraints& constraints) const override {
        return virt::VirtualItemMeasure{kVirtualItemExtent, constraints.cross_extent};
    }
    void build(std::size_t index, ui::Node& out) override {
        owner_.add_item(out, index);
    }

    SceneBuilder& owner_;
};

SceneBuilder::SceneBuilder() = default;

SceneBuilder::~SceneBuilder() = default;

std::uint64_t SceneBuilder::next_random() {
    // splitmix64: fixed seeds, so the generated text lengths and badges never differ across runs.
    std::uint64_t z = (random_state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

Node& SceneBuilder::node(Node& parent, const char* kind, const char* class_name) {
    auto child = std::make_unique<Node>(next_id_++);
    child->set_element_kind(kind);
    child->set_class_name(class_name);
    ++nodes_;
    return parent.add_child(std::move(child));
}

Node& SceneBuilder::text(Node& parent, const char* kind, const char* class_name,
                         std::string value) {
    Node& result = node(parent, kind, class_name);
    ui::PaintData data;
    data.kind = ui::PaintKind::Text;
    data.text = std::move(value);
    result.set_paint_data(std::move(data));
    return result;
}

Node& SceneBuilder::image(Node& parent, const char* class_name, std::string_view asset) {
    Node& result = node(parent, "Image", class_name);
    ui::PaintData data;
    data.kind = ui::PaintKind::Image;
    data.image.asset = asset;
    result.set_paint_data(std::move(data));
    return result;
}

void SceneBuilder::add_row(Node& parent, const char* row_class, const char* cell_class,
                           std::size_t cells, std::uint64_t row_number) {
    Node& row = node(parent, "Row", row_class);
    for (std::size_t cell = 0; cell < cells; ++cell) {
        text(row, "Text", cell_class, numbered("Record ", row_number * 16 + cell));
    }
}

void SceneBuilder::add_item(Node& item, std::size_t index) {
    item.set_id(next_id_++);
    item.set_element_kind("VirtualItem");
    item.set_class_name("virtual-item");
    item.set_item_key(index);
    ++nodes_;
    text(item, "Text", "item-title", numbered("Row ", index));
    text(item, "Text", "item-sub", numbered("group ", index % 7));
    text(item, "Text", "item-meta", numbered("value ", next_random() % 1000));
    text(item, "Text", "item-badge", numbered("#", index % 5));
}

void SceneBuilder::pad_to_target(Node& parent) {
    // Every record row is five nodes (container + four cells). Padding with whole rows keeps the
    // layout regular; the single tail row absorbs up to four leftover nodes.
    std::uint64_t row_number = 0;
    while (nodes_ + 5 <= kBenchmarkNodes) {
        add_row(parent, "records-row", "records-cell", 4, row_number++);
    }
    if (nodes_ >= kBenchmarkNodes) {
        return;
    }
    Node& tail = node(parent, "Row", "records-row");
    while (nodes_ < kBenchmarkNodes) {
        text(tail, "Text", "records-cell", numbered("tail ", nodes_));
    }
}

SceneTargets SceneBuilder::build(Node& root) {
    nodes_ = 1;
    next_id_ = 2;
    random_state_ = 0x9E3779B97F4A7C15ULL;
    root.set_element_kind("Root");
    root.set_class_name("app-root");

    Node& topbar = node(root, "Container", "topbar");
    image(topbar, "logo", kImageAsset);
    text(topbar, "Text", "brand", "Tessera");
    Node& nav = node(topbar, "Container", "nav");
    for (std::uint64_t index = 0; index < 8; ++index) {
        Node& item = node(nav, "Container", "nav-item");
        text(item, "Text", "nav-label", numbered("Section ", index));
    }

    Node& body = node(root, "Container", "body");
    Node& sidebar = node(body, "Container", "sidebar");
    sidebar.set_scrollable_y(true);
    for (std::uint64_t index = 0; index < 40; ++index) {
        Node& row = node(sidebar, "Row", "menu-row");
        text(row, "Text", "menu-label", numbered("Menu item ", index));
        text(row, "Text", "menu-badge", numbered("", 100 + index));
    }

    Node& content = node(body, "Container", "content");
    Node& toolbar = node(content, "Container", "toolbar");
    for (std::uint64_t index = 0; index < 12; ++index) {
        Node& button = node(toolbar, "Container", "button");
        text(button, "Text", "button-label", numbered("Action ", index));
    }

    Node& cards = node(content, "Container", "cards");
    SceneTargets targets;
    targets.glass_bar = &topbar;
    for (std::uint64_t index = 0; index < 20; ++index) {
        Node& card = node(cards, "Container", "card");
        image(card, "card-image", kImageAsset);
        Node& title = text(card, "Text", "card-title", numbered("Card ", index) + " summary title");
        text(card, "Text", "card-body",
             numbered("Deterministic body copy for card ", index) +
                 " rendered without a font, network or clock.");
        Node& footer = node(card, "Container", "card-footer");
        text(footer, "Text", "card-meta", numbered("owner ", next_random() % 12));
        text(footer, "Text", "card-meta", numbered("state ", next_random() % 4));
        if (index == 0) {
            targets.text_node = &title;
            targets.container_node = &card;
        }
    }

    Node& records = node(content, "Container", "records");
    for (std::uint64_t index = 0; index < 120; ++index) {
        add_row(records, "records-row", "records-cell", 4, index);
    }

    Node& virtual_list = node(content, "Container", "virtual-list");
    virtual_list.set_scrollable_y(true);
    source_ = std::make_unique<VirtualSource>(*this);
    virt::VirtualRange::Config config;
    config.capacity = 100000;
    config.pool_cap = 256;
    config.estimate_main = kVirtualItemExtent;
    config.overscan_main = 0.0f;
    config.cross_extent = 0.0f;
    config.horizontal = false;
    range_ = std::make_unique<virt::VirtualRange>(config, *source_, virtual_list);
    if (!range_->sync()) {
        throw std::runtime_error("benchmark scene: virtual list sync failed");
    }
    if (!range_->update(0.0f, kVirtualViewportExtent)) {
        throw std::runtime_error("benchmark scene: virtual list update failed");
    }

    pad_to_target(records);
    if (nodes_ != kBenchmarkNodes) {
        throw std::runtime_error("benchmark scene: node count does not match the target");
    }
    targets.nodes = nodes_;
    return targets;
}

style::CompiledStyleSheet compile_benchmark_stylesheet() {
    return style::compile_stylesheet(kStylesheet);
}

} // namespace tessera::benchmark
