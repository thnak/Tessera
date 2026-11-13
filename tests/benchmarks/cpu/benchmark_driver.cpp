// SPDX-License-Identifier: MIT

#include "benchmark_driver.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <string>
#include <utility>

namespace tessera::benchmark {
namespace {

namespace rhi = tessera::rendering::rhi;
namespace style = tessera::ui::style;

// The selectors are resolved through the captured scene tree; the benchmark never attaches or
// detaches nodes after the driver exists, so the tree stays valid.
style::ComputedStyle apply_inline(style::ComputedStyle style, const ui::Node& node,
                                  const style::PropertyRegistry& registry) {
    // Inline declarations are already bounded and borrowed by the node. `var()` substitution in an
    // inline declaration is not needed by the reference scene (its only inline edit is a plain
    // width), so the expansion here is the registry shorthand/longhand mapping only.
    for (const ui::Node::InlineStyleDeclaration& declaration : node.inline_style()) {
        const style::Declaration parsed{std::string(declaration.name),
                                        std::string(declaration.value), false};
        const style::PropertyExpansionResult expanded = registry.expand(parsed);
        for (const style::Declaration& longhand : expanded.declarations) {
            style.set(longhand.property, longhand.value);
        }
    }
    return style;
}

} // namespace

// The paint/layout provider over the resolved-style table. The table is built before the first
// frame and never reallocates after that (`resolve_all_styles` reserves the final size), so a
// lookup is a hash lookup plus the documented ComputedStyle copy.
struct BenchmarkDriver::CachedStyles final : ui::paint::StyleProvider {
    explicit CachedStyles(std::unordered_map<ui::NodeId, style::ComputedStyle>& table)
        : table(&table) {}

    [[nodiscard]] style::ComputedStyle compute(const ui::Node& node) override {
        const auto found = table->find(node.id());
        return found == table->end() ? style::ComputedStyle{} : found->second;
    }

    std::unordered_map<ui::NodeId, style::ComputedStyle>* table = nullptr;
};

namespace {

ui::frame::FramePipelineConfig pipeline_config(bool cull_offscreen_paint) {
    ui::frame::FramePipelineConfig config;
    config.cull_offscreen_paint = cull_offscreen_paint;
    return config;
}

} // namespace

BenchmarkDriver::BenchmarkDriver(ui::Node& root, style::CompiledStyleSheet sheet,
                                 ui::layout::LayoutViewport viewport, bool cull_offscreen_paint)
    : root_(&root),
      sheet_(std::move(sheet)),
      style_tree_(root),
      provider_(std::make_unique<CachedStyles>(styles_)),
      loop_(clock_),
      damage_(rendering::DamageConfig{rhi::Extent2D{static_cast<std::uint32_t>(viewport.width),
                                                    static_cast<std::uint32_t>(viewport.height)},
                                      8}),
      pipeline_(animations_, loop_, tracker_, damage_, dirty_,
                pipeline_config(cull_offscreen_paint)),
      engine_(),
      scheduler_(engine_),
      viewport_(viewport) {}

BenchmarkDriver::~BenchmarkDriver() = default;

style::ComputedStyle BenchmarkDriver::compute_one(const ui::Node& node,
                                                  const style::ComputedStyle* parent) {
    // The style stage is the production resolution (`CompiledStyleSheet::compute`, which inherits
    // and materializes the registry's initial longhands), so the measured "first style+layout"
    // number includes what the engine really does for every node.
    const style::ComputedStyle materialized = sheet_.compute(*style_tree_.view(node), parent);
    const style::ComputedStyle styled = apply_inline(materialized, node, sheet_.registry());
    // The resolved table drops longhands that still equal their registry initial value. Readers of
    // a computed style treat an absent longhand as its initial value (the engine's stubs and every
    // paint/layout test rely on it), so this is the same style semantically while keeping 12 000
    // entries at a few megabytes instead of ~100 MB of default strings. The copy cost is paid once
    // per node here, not per layout or paint consumer.
    style::ComputedStyle stripped;
    const style::PropertyRegistry& registry = sheet_.registry();
    for (const auto& [name, value] : styled.properties()) {
        const style::PropertyInfo* info = registry.find(name);
        if (info != nullptr && info->initial == value) {
            continue;
        }
        stripped.set(name, value);
    }
    return stripped;
}

void BenchmarkDriver::resolve_all_styles() {
    // Past the reference scene's node count, so inserting every resolved style never rehashes. The
    // recursion below passes the parent style by reference to the local, never into the table, so a
    // rehash could not invalidate it; reserving just keeps lookups and copies stable.
    styles_.reserve(16384);
    // Depth-first so every node's parent style is the already-resolved one, not a re-walk of the
    // ancestor chain: one cascade per node.
    const auto visit = [&](auto&& self, ui::Node& node,
                           const style::ComputedStyle* parent) -> void {
        const style::ComputedStyle style = compute_one(node, parent);
        for (const auto& child : node.children()) {
            self(self, *child, &style);
        }
        styles_.emplace(node.id(), style);
    };
    visit(visit, *root_, nullptr);
}

void BenchmarkDriver::resolve_subtree(ui::Node& node) {
    const style::ComputedStyle* parent = nullptr;
    style::ComputedStyle parent_style;
    if (const ui::Node* parent_node = node.parent(); parent_node != nullptr) {
        const auto found = styles_.find(parent_node->id());
        if (found != styles_.end()) {
            parent_style = found->second;
            parent = &parent_style;
        }
    }
    const auto visit = [&](auto&& self, ui::Node& current,
                           const style::ComputedStyle* parent_style_ptr) -> void {
        const style::ComputedStyle style = compute_one(current, parent_style_ptr);
        for (const auto& child : current.children()) {
            self(self, *child, &style);
        }
        styles_.insert_or_assign(current.id(), style);
    };
    visit(visit, node, parent);
}

void BenchmarkDriver::resolve_dirty_styles() {
    // `DirtyPass` already collected this frame's dirt; only a node whose own style domain is dirty
    // needs a new cascade (its descendants inherit whatever changed).
    for (const ui::DirtyNode& entry : dirty_.nodes()) {
        if (entry.node != nullptr && ui::has_flag(entry.flags, ui::Dirty::Style)) {
            resolve_subtree(*entry.node);
        }
    }
}

namespace {

ui::layout::LayoutInput build_layout_input(const ui::Node& node, ui::paint::StyleProvider& styles) {
    ui::layout::LayoutInput input;
    input.id = node.id();
    input.style = styles.compute(node);
    const auto& paint = node.paint_data();
    if (paint && paint->kind == ui::PaintKind::Text) {
        input.text = paint->text;
    }
    for (const auto& child : node.children()) {
        input.children.push_back(build_layout_input(*child, styles));
    }
    return input;
}

} // namespace

void BenchmarkDriver::run_layout_pass() {
    const ui::layout::LayoutInput input = build_layout_input(*root_, *provider_);
    // The scheduler reuses the last result while the layout-input signature is unchanged, exactly
    // like AppHost: a style-only edit that cannot move boxes skips Yoga but still re-syncs styles.
    last_layout_ = scheduler_.update(input, viewport_, true);
    last_applied_ = ui::layout::apply_layout(last_layout_, *root_);
}

void BenchmarkDriver::request_full_repaint() {
    full_repaint_pending_ = true;
}

FrameWork BenchmarkDriver::begin_frame() {
    FrameWork frame;
    frame.tick = pipeline_.tick(*root_, 0.0);
    if (full_repaint_pending_) {
        damage_.force_full_redraw();
        full_repaint_pending_ = false;
    }
    if (!styles_resolved_) {
        resolve_all_styles();
        styles_resolved_ = true;
        frame.geometry = true;
    } else if (frame.tick.needs_style) {
        resolve_dirty_styles();
        frame.geometry = true;
    }
    if (frame.geometry || frame.tick.needs_layout) {
        run_layout_pass();
        frame.layout = last_applied_;
        frame.sync = pipeline_.sync_style(*root_, *provider_, frame.tick, true);
    }
    frame.render = pipeline_.finish_frame(*root_);
    last_render_ = frame.render;
    return frame;
}

ui::frame::FramePaint BenchmarkDriver::paint(ui::paint::PaintSink& sink, bool force) {
    return pipeline_.paint_frame(*root_, *provider_, sink, last_render_.redraw_plan, force);
}

void BenchmarkDriver::end_frame() {
    dirty_.clear(*root_, ui::Dirty::All);
}

} // namespace tessera::benchmark
