// SPDX-License-Identifier: MIT
//
// Allocations per frame (issue #701, step 1): how many global `operator new` calls and bytes one
// steady-state `AppHost::begin_frame` + `paint` makes, per scenario and scene size, after warm-up.
// docs/specs/memory-model-v1.md section 2 says "no malloc/new in steady-state frame code after
// warm-up"; this prints the number that claim is measured against. It is a measurement, not a gate:
// nothing here asserts a bound until the numbers have been read.
//
// `--classcheck` is a one-frame repro, not a measurement: a class change that alters only a paint
// property (background-color) restyles the tree but plans no redraw, so the plan-driven paint draws
// nothing while a forced paint draws the new color (a stale frame on screen).
//
// The allocation counters come from a replaced global `operator new`, so every allocation in the
// process counts, including the ones the standard library makes for the code under test. Counting
// is switched on only around the measured frames. No GPU, no window, no font, no wall clock.
//
// Usage: frame_alloc_probe [--frames N] [--warmup N] [--only SCENARIO --cards 10|200] |
// --classcheck | --computecheck Output: one line per scenario, `name allocs/frame(min,mean,max)
// bytes/frame(mean)`.

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

bool g_counting = false;
std::size_t g_allocations = 0;
std::size_t g_bytes = 0;

void count(std::size_t size) {
    if (g_counting) {
        ++g_allocations;
        g_bytes += size;
    }
}

} // namespace

void* operator new(std::size_t size) {
    count(size);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::paint::TextDraw;

class CountingSink final : public tessera::ui::paint::PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {
        ++commands;
    }
    void fill_rounded_rect(const Rect&, const Color&, float) override {
        ++commands;
    }
    void draw_text(const TextDraw&) override {
        ++commands;
    }
    std::size_t commands = 0;
};

PaintData text_payload(std::string text) {
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = std::move(text);
    return data;
}

struct Scene {
    std::unique_ptr<Node> root = std::make_unique<Node>(1);
    std::vector<Node*> cards;
    std::vector<Node*> labels;

    explicit Scene(int card_count) {
        std::uint64_t id = 2;
        for (int i = 0; i < card_count; ++i) {
            Node& card = root->add_child(std::make_unique<Node>(id++));
            card.set_element_kind("View");
            card.set_class_name("card");
            cards.push_back(&card);
            for (int j = 0; j < 2; ++j) {
                Node& label = card.add_child(std::make_unique<Node>(id++));
                label.set_element_kind("Text");
                label.set_paint_data(
                    text_payload("Item " + std::to_string(i) + "." + std::to_string(j)));
                labels.push_back(&label);
            }
        }
    }
};

constexpr const char* kSheet = R"(
.card { width: 120px; height: 40px; padding: 4px; margin: 2px; background-color: #1e293b; }
.card.hot { background-color: #334155; }
Text { font-size: 14px; color: #e2e8f0; }
)";

enum class Action { Idle, PaintDirty, TextChange, ClassToggle, LayoutDirty };

const char* name_of(Action action) {
    switch (action) {
    case Action::Idle:
        return "idle";
    case Action::PaintDirty:
        return "paint-dirty";
    case Action::TextChange:
        return "text-change";
    case Action::ClassToggle:
        return "class-toggle";
    case Action::LayoutDirty:
        return "layout-dirty";
    }
    return "?";
}

void apply(Action action, Scene& scene, int frame) {
    Node& card = *scene.cards[static_cast<std::size_t>(frame) % scene.cards.size()];
    Node& label = *scene.labels[static_cast<std::size_t>(frame) % scene.labels.size()];
    switch (action) {
    case Action::Idle:
        break;
    case Action::PaintDirty:
        label.mark_dirty(Dirty::Paint);
        break;
    case Action::TextChange:
        label.set_paint_data(text_payload("Tick " + std::to_string(frame)));
        break;
    case Action::ClassToggle:
        card.set_class_name((frame % 2) == 0 ? "card hot" : "card");
        break;
    case Action::LayoutDirty:
        card.mark_dirty(Dirty::Layout);
        break;
    }
}

void measure(int card_count, Action action, int warmup, int frames) {
    Scene scene(card_count);
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 1280.0f;
    config.height = 720.0f;
    tessera::app::AppHost host(*scene.root, tessera::ui::style::compile_stylesheet(kSheet), loop,
                               config);
    CountingSink sink;

    std::size_t min_allocs = static_cast<std::size_t>(-1);
    std::size_t max_allocs = 0;
    std::size_t total_allocs = 0;
    std::size_t total_bytes = 0;
    std::size_t begin_total = 0;
    int style_rebuilds = 0;
    int should_render = 0;
    int needs_layout = 0;
    double total_ms = 0.0;
    std::size_t painted_nodes = 0;
    std::size_t culled_nodes = 0;
    for (int frame = 0; frame < warmup + frames; ++frame) {
        apply(action, scene, frame);
        const bool measured = frame >= warmup;
        if (measured) {
            g_allocations = 0;
            g_bytes = 0;
            g_counting = true;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const std::size_t before_begin = g_allocations;
        const tessera::app::HostFrame hf = host.begin_frame(1.0 / 60.0);
        const std::size_t after_begin = g_allocations;
        const auto fp = host.paint(sink);
        host.end_frame();
        if (measured) {
            total_ms +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            begin_total += after_begin - before_begin;
            painted_nodes += fp.stats.nodes;
            culled_nodes += fp.stats.culled_nodes;
            style_rebuilds += hf.style_rebuilt ? 1 : 0;
            should_render += hf.render.should_render ? 1 : 0;
            needs_layout += hf.tick.needs_layout ? 1 : 0;
            g_counting = false;
            min_allocs = std::min(min_allocs, g_allocations);
            max_allocs = std::max(max_allocs, g_allocations);
            total_allocs += g_allocations;
            total_bytes += g_bytes;
        }
    }
    std::fflush(stdout);
    std::printf(
        "%-13s cards=%-4d nodes=%-5zu allocs/frame min=%zu mean=%.1f max=%zu  bytes/frame=%.0f\n",
        name_of(action), card_count, 1 + scene.cards.size() * 3, min_allocs,
        static_cast<double>(total_allocs) / frames, max_allocs,
        static_cast<double>(total_bytes) / frames);
    std::printf("              begin_frame=%.1f paint=%.1f  style_rebuilt=%d/%d needs_layout=%d/%d "
                "should_render=%d/%d\n",
                static_cast<double>(begin_total) / frames,
                static_cast<double>(total_allocs - begin_total) / frames, style_rebuilds, frames,
                needs_layout, frames, should_render, frames);
    const auto& cache = host.paint_cache_stats();
    std::printf("              ms/frame=%.3f painted_nodes/frame=%.1f culled=%.1f  cache hits=%llu "
                "misses=%llu\n",
                total_ms / frames, static_cast<double>(painted_nodes) / frames,
                static_cast<double>(culled_nodes) / frames,
                static_cast<unsigned long long>(cache.hits),
                static_cast<unsigned long long>(cache.misses));
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    int frames = 120;
    int warmup = 60;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            frames = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--warmup") == 0) {
            warmup = std::atoi(argv[i + 1]);
        }
    }
    std::printf("# frame_alloc_probe: %d warm-up, %d measured frames per row\n", warmup, frames);
    if (argc > 1 && std::strcmp(argv[1], "--computecheck") == 0) {
        // Where do the allocations of one computed style go? Each row is the allocation count of
        // one call after a warm-up call, so a one-time cache fill is not counted.
        const auto sheet = tessera::ui::style::compile_stylesheet(kSheet);
        const auto count_of = [](const char* label, auto&& fn) {
            fn();
            g_allocations = 0;
            g_bytes = 0;
            g_counting = true;
            fn();
            g_counting = false;
            std::printf("%-46s %8zu allocs %9zu bytes\n", label, g_allocations, g_bytes);
        };
        Scene scene(3);
        tessera::ui::style::SceneStyleTree tree(*scene.root);
        const Node& root_node = *scene.root;
        const Node& card_node = *scene.cards[0];
        const Node& label_node = *scene.labels[0];
        count_of("tree.compute(root)   depth 1", [&] { (void)tree.compute(sheet, root_node); });
        count_of("tree.compute(card)   depth 2", [&] { (void)tree.compute(sheet, card_node); });
        count_of("tree.compute(label)  depth 3", [&] { (void)tree.compute(sheet, label_node); });
        count_of("registry.initial_style()", [&] { (void)sheet.registry().initial_style(); });
        const tessera::ui::style::CascadeTable empty;
        count_of("compute_style(nullptr, empty cascade)",
                 [&] { (void)tessera::ui::style::compute_style(nullptr, empty); });
        tessera::ui::style::ComputedStyle filled = sheet.registry().initial_style();
        count_of("compute_style(parent=filled, empty cascade)",
                 [&] { (void)tessera::ui::style::compute_style(&filled, empty); });
        count_of("copy of a filled ComputedStyle", [&] {
            tessera::ui::style::ComputedStyle copy = filled;
            (void)copy;
        });
        std::printf("entries in a computed style: %zu\n", filled.properties().size());
        std::size_t long_values = 0;
        for (const auto& [k, v] : filled.properties()) {
            long_values += v.size() > 15 ? 1 : 0;
        }
        std::printf("values longer than the SSO limit (15): %zu\n", long_values);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--classcheck") == 0) {
        struct ColorSink final : tessera::ui::paint::PaintSink {
            void fill_rect(const Rect&, const Color& c) override {
                if (n == 0) {
                    first_c = c;
                }
                last = c;
                ++n;
            }
            void fill_rounded_rect(const Rect&, const Color& c, float) override {
                last = c;
                ++n;
            }
            void draw_text(const TextDraw&) override {}
            Color last{};
            Color first_c{};
            int n = 0;
        };
        Scene scene(3);
        tessera::runtime::ManualClock clock;
        tessera::runtime::RuntimeLoop loop(clock);
        tessera::app::HostConfig config;
        config.width = 800.0f;
        config.height = 600.0f;
        tessera::app::AppHost host(*scene.root, tessera::ui::style::compile_stylesheet(kSheet),
                                   loop, config);
        ColorSink first;
        (void)host.begin_frame(1.0 / 60.0);
        (void)host.paint(first);
        host.end_frame();
        scene.cards[0]->set_class_name("card hot");
        const auto hf = host.begin_frame(1.0 / 60.0);
        ColorSink after;
        const auto fp = host.paint(after);
        std::printf("class change: style=%d rebuilt=%d render=%d painted=%d fills=%d "
                    "lastcolor=%.3f,%.3f,%.3f (want 0.200,0.255,0.333)\n",
                    hf.tick.needs_style, hf.style_rebuilt, hf.render.should_render, fp.painted,
                    after.n, after.last.r, after.last.g, after.last.b);
        ColorSink forced;
        (void)host.paint(forced, true);
        std::printf("forced paint: fills=%d card0 fill=%.3f,%.3f,%.3f\n", forced.n,
                    forced.first_c.r, forced.first_c.g, forced.first_c.b);
        return 0;
    }
    // `--only <scenario> --cards N`: one row, for running the probe under a sampling profiler.
    const char* only = nullptr;
    int only_cards = 200;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--only") == 0) {
            only = argv[i + 1];
        } else if (std::strcmp(argv[i], "--cards") == 0) {
            only_cards = std::atoi(argv[i + 1]);
        }
    }
    for (int cards : {10, 200}) {
        for (Action action : {Action::Idle, Action::PaintDirty, Action::TextChange,
                              Action::ClassToggle, Action::LayoutDirty}) {
            if (only != nullptr &&
                (std::strcmp(only, name_of(action)) != 0 || cards != only_cards)) {
                continue;
            }
            measure(cards, action, warmup, frames);
        }
    }
    return 0;
}
