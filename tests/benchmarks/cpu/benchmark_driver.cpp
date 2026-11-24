// SPDX-License-Identifier: MIT

#include "benchmark_driver.hpp"
#include "tessera/ui/layout/layout_signature.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <chrono>
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
// lookup is an array index.
struct BenchmarkDriver::CachedStyles final : ui::paint::StyleProvider {
    explicit CachedStyles(const StyleTable& table)
        : table(&table) {}

    [[nodiscard]] style::ComputedStyle compute(const ui::Node& node) override {
        const style::ComputedStyle* found = table->get(node.id());
        return found == nullptr ? style::ComputedStyle{} : *found;
    }

    // The table never reallocates after the first frame, so a reference into it stays valid; this
    // is what lets the layout input fill without copying through `compute`, like the host does.
    [[nodiscard]] const style::ComputedStyle& resolve(const ui::Node& node) override {
        static const style::ComputedStyle empty;
        const style::ComputedStyle* found = table->get(node.id());
        return found == nullptr ? empty : *found;
    }

    const StyleTable* table = nullptr;
};

BenchmarkDriver::BenchmarkDriver(ui::Node& root, style::CompiledStyleSheet sheet,
                                 ui::layout::LayoutViewport viewport)
    : root_(&root),
      sheet_(std::move(sheet)),
      style_tree_(root),
      provider_(std::make_unique<CachedStyles>(styles_)),
      loop_(clock_),
      damage_(rendering::DamageConfig{rhi::Extent2D{static_cast<std::uint32_t>(viewport.width),
                                                    static_cast<std::uint32_t>(viewport.height)},
                                      8}),
      pipeline_(animations_, loop_, tracker_, damage_, dirty_, ui::frame::FramePipelineConfig{}),
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
    // A computed style is already a delta over the registry's initial values (issue #735, slice F),
    // so it holds only the longhands that differ from them: the ~100-entry table this used to strip
    // by hand is the shared defaults table now, and the copy below is a handful of entries.
    return apply_inline(materialized, node, sheet_.registry());
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
        styles_.put(node.id(), style);
    };
    visit(visit, *root_, nullptr);
}

void BenchmarkDriver::resolve_subtree(ui::Node& node) {
    const style::ComputedStyle* parent = nullptr;
    style::ComputedStyle parent_style;
    if (const ui::Node* parent_node = node.parent(); parent_node != nullptr) {
        if (const style::ComputedStyle* found = styles_.get(parent_node->id()); found != nullptr) {
            parent_style = *found;
            parent = &parent_style;
        }
    }
    const auto visit = [&](auto&& self, ui::Node& current,
                           const style::ComputedStyle* parent_style_ptr) -> void {
        const style::ComputedStyle style = compute_one(current, parent_style_ptr);
        for (const auto& child : current.children()) {
            self(self, *child, &style);
        }
        styles_.put(current.id(), style);
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

// Rewrites `input` in place, as AppHost does (issue #701): the tree is retained between passes so a
// pass reuses the elements and their children vectors instead of reallocating every one.
void fill_layout_input(const ui::Node& node, ui::paint::StyleProvider& styles,
                       ui::layout::LayoutInput& input,
                       ui::layout::LayoutSignatureBuilder& signature) {
    input.id = node.id();
    input.text.clear();
    input.text_wrap = false;
    input.style = styles.resolve(node);
    const auto& paint = node.paint_data();
    if (paint && paint->kind == ui::PaintKind::Text) {
        input.text = paint->text;
        // Layout's text-fit fact must use paint's own wrap decision (issue #490).
        input.text_wrap = paint->wrap == ui::PaintWrap::Wrap;
    }
    signature.enter(input); // folded while the node is hot, as AppHost does
    if (input.children.capacity() < node.children().size()) {
        input.children.reserve(node.children().size());
    }
    std::size_t used = 0;
    for (const auto& child : node.children()) {
        if (used == input.children.size()) {
            input.children.emplace_back();
        }
        fill_layout_input(*child, styles, input.children[used], signature);
        ++used;
    }
    input.children.resize(used);
    signature.leave(used);
}

} // namespace

namespace {

using PhaseClock = std::chrono::steady_clock;

double phase_ms(PhaseClock::time_point begin, PhaseClock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

} // namespace

void BenchmarkDriver::run_layout_pass() {
    const PhaseClock::time_point input_begin = PhaseClock::now();
    ui::layout::LayoutSignatureBuilder signature(viewport_);
    fill_layout_input(*root_, *provider_, layout_input_, signature);
    const ui::layout::LayoutInput& input = layout_input_;
    const PhaseClock::time_point input_end = PhaseClock::now();
    phases_.layout_input_ms = phase_ms(input_begin, input_end);
    // The scheduler reuses the last result while the layout-input signature is unchanged, exactly
    // like AppHost: a style-only edit that cannot move boxes skips Yoga but still re-syncs styles.
    last_layout_ = scheduler_.update(input, viewport_, true, signature.value());
    const PhaseClock::time_point engine_end = PhaseClock::now();
    phases_.layout_engine_ms = phase_ms(input_end, engine_end);
    last_applied_ = ui::layout::apply_layout(last_layout_, *root_, resized_nodes_);
    phases_.apply_layout_ms = phase_ms(engine_end, PhaseClock::now());
}

void BenchmarkDriver::request_full_repaint() {
    full_repaint_pending_ = true;
}

FrameWork BenchmarkDriver::begin_frame() {
    FrameWork frame;
    phases_ = FramePhases{};
    const PhaseClock::time_point tick_begin = PhaseClock::now();
    frame.tick = pipeline_.tick(*root_, 0.0);
    const PhaseClock::time_point tick_end = PhaseClock::now();
    phases_.tick_ms = phase_ms(tick_begin, tick_end);
    if (full_repaint_pending_) {
        damage_.force_full_redraw();
        full_repaint_pending_ = false;
    }
    bool scoped = false;
    sync_roots_.clear();
    if (!styles_resolved_) {
        resolve_all_styles();
        styles_resolved_ = true;
        frame.geometry = true;
    } else if (frame.tick.needs_style) {
        resolve_dirty_styles();
        frame.geometry = true;
        scoped = true;
    } else {
        // Layout-only frame: no node restyled, so only a resized box can owe the sync anything.
        scoped = true;
    }
    const PhaseClock::time_point styles_end = PhaseClock::now();
    phases_.styles_ms = phase_ms(tick_end, styles_end);
    if (frame.geometry || frame.tick.needs_layout) {
        run_layout_pass();
        frame.layout = last_applied_;
        const PhaseClock::time_point sync_begin = PhaseClock::now();
        // The reference scene never attaches or detaches nodes after the driver exists, so once the
        // first frame resolved every style a scoped sync (restyled roots plus resized boxes) sees
        // everything the full walk would, which is the host's scoped path (issue #735).
        if (scoped) {
            for (const ui::DirtyNode& entry : dirty_.nodes()) {
                if (entry.node != nullptr && ui::has_flag(entry.flags, ui::Dirty::Style)) {
                    sync_roots_.push_back(entry.node);
                }
            }
            sync_roots_.insert(sync_roots_.end(), resized_nodes_.begin(), resized_nodes_.end());
            frame.sync =
                pipeline_.sync_style_scoped(*root_, *provider_, sync_roots_, frame.tick, true);
        } else {
            frame.sync = pipeline_.sync_style(*root_, *provider_, frame.tick, true);
        }
        phases_.sync_style_ms = phase_ms(sync_begin, PhaseClock::now());
    }
    const PhaseClock::time_point finish_begin = PhaseClock::now();
    frame.render = pipeline_.finish_frame(*root_);
    phases_.finish_ms = phase_ms(finish_begin, PhaseClock::now());
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
