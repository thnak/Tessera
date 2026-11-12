// SPDX-License-Identifier: MIT
//
// FramePipeline-owned per-node paint cache (docs/design/dirty-rendering.md section 5.4). The
// production paint path records one node's own sink calls and replays them while the node is clean
// and every recorded input is unchanged, so a settled frame re-records nothing. Cached output must
// be byte-identical to the uncached pipeline, a dirty node must re-record, and a style/tree
// generation change must drop the records instead of replaying the previous generation.
// Deterministic: no GPU, wall clock, system font, network or real window.

#include "paint_cache_test_support.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using paint_cache_test::build_rich_scene;
using paint_cache_test::Call;
using paint_cache_test::clear_all;
using paint_cache_test::count_nodes;
using paint_cache_test::CountingStyles;
using paint_cache_test::RecordingSink;
using paint_cache_test::same_calls;
using paint_cache_test::same_color;
using paint_cache_test::same_rect;
using paint_cache_test::styled;
using tessera::app::AppHost;
using tessera::app::HostConfig;
using tessera::app::HostFrame;
using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::rgba8;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::FramePaint;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FramePipelineConfig;
using tessera::ui::frame::FrameRender;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::PaintCacheStats;
using tessera::ui::paint::PaintStats;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_paint_cache: " << message << "\n";
    }
    return condition;
}

FramePipelineConfig pipeline_config(bool cache) {
    FramePipelineConfig config;
    config.paint_cache = cache;
    return config;
}

// One rich scene (every sink call kind) plus the style stub that feeds it.
struct Scene {
    Node root{1};
    CountingStyles styles;

    Scene() {
        build_rich_scene(root, styles);
    }
};

// The frame-stack state one pipeline needs; `cache` selects FramePipelineConfig::paint_cache.
struct Harness {
    ManualClock clock;
    RuntimeLoop loop{clock};
    AnimationRuntime animations;
    DamageAccumulator damage{DamageConfig{Extent2D{240, 180}, 16}};
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline;

    explicit Harness(bool cache)
        : pipeline(animations, loop, tracker, damage, dirty, pipeline_config(cache)) {}
};

Node* find_node(Node& node, NodeId id) {
    if (node.id() == id) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (Node* found = find_node(*child, id)) {
            return found;
        }
    }
    return nullptr;
}

// The node-2 style of build_rich_scene with a swapped background: a paint-only edit.
tessera::ui::style::ComputedStyle text_node_style(const char* background) {
    return styled({{"background-color", background},
                   {"padding-left", "4px"},
                   {"padding-top", "3px"},
                   {"font-size", "16px"},
                   {"color", "#ffffff"}});
}

// Every PaintStats field except `nodes`, which counts resolved styles and therefore drops on a hit
// (the cache replays the recorded draw counters instead).
bool same_draw_composition(const PaintStats& lhs, const PaintStats& rhs) {
    return lhs.backgrounds == rhs.backgrounds && lhs.borders == rhs.borders &&
           lhs.hidden_subtrees == rhs.hidden_subtrees && lhs.clips == rhs.clips &&
           lhs.texts == rhs.texts && lhs.vectors == rhs.vectors && lhs.images == rhs.images &&
           lhs.gradients == rhs.gradients && lhs.shadows == rhs.shadows &&
           lhs.transforms == rhs.transforms;
}

bool has_fill(const std::vector<Call>& calls, const Rect& rect, const Color& color) {
    for (const Call& call : calls) {
        if ((call.kind == Call::Kind::Rect || call.kind == Call::Kind::Rounded) &&
            same_rect(call.rect, rect) && same_color(call.color, color)) {
            return true;
        }
    }
    return false;
}

const Call* find_text(const std::vector<Call>& calls, const char* bytes) {
    for (const Call& call : calls) {
        if (call.kind == Call::Kind::Text && call.text == bytes) {
            return &call;
        }
    }
    return nullptr;
}

// One authoritative frame followed by a forced full paint: the tests ask what the paint pass emits,
// not what it damages, so a settled (not due) frame must still paint for the replay to be observed.
FramePaint paint_full(Harness& harness, Scene& scene, RecordingSink& sink) {
    (void)harness.pipeline.tick(scene.root, 0.016);
    (void)harness.pipeline.finish_frame(scene.root);
    return harness.pipeline.paint_frame(scene.root, scene.styles, sink, true);
}

// A settled frame re-records nothing: every visible node replays, the counters move exactly as a
// full hit predicts, and the emitted sequence equals the uncached pipeline's, field by field.
bool test_settled_frame_replays_everything() {
    Scene scene;
    Harness harness(true);
    RecordingSink sink;

    const FramePaint cold = paint_full(harness, scene, sink);
    const PaintCacheStats cold_stats = harness.pipeline.paint_cache_stats();
    const std::vector<Call> cold_calls = sink.calls;
    const std::size_t visible = count_nodes(scene.root);
    if (!expect(cold.painted && cold.stats.nodes == visible,
                "a cold paint resolves every visible node") ||
        !expect(cold_stats.hits == 0 && cold_stats.misses == visible,
                "a cold paint misses every node once") ||
        !expect(cold_stats.recorded_draws > 0 && cold_stats.replayed_draws == 0,
                "a cold paint records instead of replaying") ||
        !expect(cold_stats.entries == visible, "one record per visible node")) {
        return false;
    }

    clear_all(scene.root); // what AppHost::end_frame() does after a painted frame
    (void)harness.pipeline.tick(scene.root, 0.016);
    const FrameRender settled = harness.pipeline.finish_frame(scene.root);
    sink.calls.clear();
    const FramePaint warm = paint_full(harness, scene, sink);
    const PaintCacheStats warm_stats = harness.pipeline.paint_cache_stats();

    if (!expect(!settled.should_render, "the settled frame has nothing to damage") ||
        !expect(warm.painted, "a forced paint of the settled scene still paints") ||
        !expect(warm.stats.nodes == 0, "no node's style is resolved on a full hit") ||
        !expect(warm_stats.hits - cold_stats.hits == visible,
                "every visible node is a cache hit") ||
        !expect(warm_stats.misses == cold_stats.misses, "no lookup misses") ||
        !expect(warm_stats.recorded_draws == cold_stats.recorded_draws,
                "a settled frame re-records no draw") ||
        !expect(warm_stats.replayed_draws - cold_stats.replayed_draws == cold_stats.recorded_draws,
                "every recorded call is replayed exactly once") ||
        !expect(warm_stats.entries == visible, "the records are kept, not re-inserted") ||
        !expect(same_draw_composition(warm.stats, cold.stats),
                "the replayed draw composition matches the cold paint")) {
        return false;
    }

    // The uncached pipeline must emit the identical call sequence, field by field.
    Scene uncached_scene;
    Harness uncached(false);
    RecordingSink uncached_sink;
    (void)paint_full(uncached, uncached_scene, uncached_sink);
    return expect(same_calls(sink.calls, uncached_sink.calls),
                  "the replayed sequence equals the uncached pipeline's") &&
           expect(same_calls(cold_calls, uncached_sink.calls),
                  "the recording pass itself forwarded the uncached sequence");
}

// One node's paint property changed and marked dirty: exactly that node misses and re-records, the
// rest replay, and the output carries the change and equals a cold paint of the new state.
bool test_one_dirty_node_re_records() {
    Scene scene;
    Harness harness(true);
    RecordingSink sink;
    (void)paint_full(harness, scene, sink);
    const PaintCacheStats settled = harness.pipeline.paint_cache_stats();
    const std::size_t visible = count_nodes(scene.root);
    clear_all(scene.root); // what AppHost::end_frame() does after a painted frame

    Node* text_node = find_node(scene.root, 2);
    if (!expect(text_node != nullptr, "the rich scene has the text node")) {
        return false;
    }
    scene.styles.set(2, text_node_style("#00ff00"));
    text_node->mark_dirty(Dirty::Paint);

    sink.calls.clear();
    const FramePaint paint = paint_full(harness, scene, sink);
    const PaintCacheStats changed = harness.pipeline.paint_cache_stats();
    if (!expect(paint.painted, "the changed frame paints") ||
        !expect(paint.stats.nodes == 1, "exactly one node resolves its style") ||
        !expect(changed.hits - settled.hits == visible - 1, "every other node still hits") ||
        !expect(changed.misses - settled.misses == 1, "exactly one lookup misses") ||
        !expect(changed.recorded_draws > settled.recorded_draws,
                "the dirty node re-records its emission") ||
        !expect(has_fill(sink.calls, Rect{10.0f, 12.0f, 80.0f, 40.0f}, rgba8(0, 255, 0)),
                "the new background color is painted")) {
        return false;
    }

    Scene expected_scene;
    expected_scene.styles.set(2, text_node_style("#00ff00"));
    Harness expected(false);
    RecordingSink expected_sink;
    (void)paint_full(expected, expected_scene, expected_sink);
    return expect(same_calls(sink.calls, expected_sink.calls),
                  "the partially replayed frame equals a cold paint of the new state");
}

// invalidate_paint_cache() drops every record (also the correctness hook for a style/tree
// generation change) and the next paint re-records the same sequence the uncached pass emits.
bool test_invalidate_drops_every_record() {
    Scene scene;
    Harness harness(true);
    RecordingSink sink;
    (void)paint_full(harness, scene, sink);
    clear_all(scene.root);
    (void)paint_full(harness, scene, sink);
    const PaintCacheStats warm = harness.pipeline.paint_cache_stats();
    const std::size_t visible = count_nodes(scene.root);
    if (!expect(warm.entries == visible, "the settled cache holds one record per node")) {
        return false;
    }

    harness.pipeline.invalidate_paint_cache();
    const PaintCacheStats invalidated = harness.pipeline.paint_cache_stats();
    if (!expect(invalidated.entries == 0 && invalidated.bytes == 0,
                "invalidate_paint_cache drops every entry") ||
        !expect(invalidated.invalidations - warm.invalidations == 1,
                "one invalidation is counted")) {
        return false;
    }

    sink.calls.clear();
    const FramePaint repainted = paint_full(harness, scene, sink);
    const PaintCacheStats replayed = harness.pipeline.paint_cache_stats();
    if (!expect(repainted.painted && repainted.stats.nodes == visible,
                "the next paint resolves every node again") ||
        !expect(replayed.hits == invalidated.hits, "a dropped cache cannot hit") ||
        !expect(replayed.misses - invalidated.misses == visible, "every node misses once") ||
        !expect(replayed.recorded_draws - invalidated.recorded_draws == warm.recorded_draws,
                "every node re-records its emission") ||
        !expect(replayed.entries == visible, "the records are rebuilt")) {
        return false;
    }

    Scene uncached_scene;
    Harness uncached(false);
    RecordingSink uncached_sink;
    (void)paint_full(uncached, uncached_scene, uncached_sink);
    return expect(same_calls(sink.calls, uncached_sink.calls),
                  "the re-recorded sequence equals the uncached pipeline's");
}

struct Step {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::size_t calls = 0;
};

// The same script as above, reduced to per-paint counter deltas plus the emitted sequence, so two
// runs can be compared step by step (no clock, address or iteration-order dependence).
std::vector<Step> run_script(std::vector<std::vector<Call>>& sequences) {
    Scene scene;
    Harness harness(true);
    RecordingSink sink;
    std::vector<Step> steps;
    PaintCacheStats previous = harness.pipeline.paint_cache_stats();

    const auto record = [&](PaintCacheStats now) {
        steps.push_back(Step{now.hits - previous.hits, now.misses - previous.misses,
                             now.evictions - previous.evictions, sink.calls.size()});
        sequences.push_back(sink.calls);
        sink.calls.clear();
        previous = now;
    };

    (void)paint_full(harness, scene, sink); // cold record
    record(harness.pipeline.paint_cache_stats());

    clear_all(scene.root);
    (void)paint_full(harness, scene, sink); // settled replay
    record(harness.pipeline.paint_cache_stats());

    scene.styles.set(2, text_node_style("#0000ff"));
    find_node(scene.root, 2)->mark_dirty(Dirty::Paint);
    (void)paint_full(harness, scene, sink); // one dirty node
    record(harness.pipeline.paint_cache_stats());

    harness.pipeline.invalidate_paint_cache();
    (void)paint_full(harness, scene, sink); // generation change
    record(harness.pipeline.paint_cache_stats());
    return steps;
}

bool test_determinism() {
    std::vector<std::vector<Call>> first_sequences;
    std::vector<std::vector<Call>> second_sequences;
    const std::vector<Step> first = run_script(first_sequences);
    const std::vector<Step> second = run_script(second_sequences);
    Scene probe;
    const std::size_t visible = count_nodes(probe.root);
    if (!expect(first.size() == 4 && second.size() == 4, "the script has four paints") ||
        !expect(first[0].misses == visible && first[0].hits == 0, "step 1 records everything") ||
        !expect(first[1].hits == visible && first[1].misses == 0, "step 2 replays every node") ||
        !expect(first[2].hits == visible - 1 && first[2].misses == 1,
                "step 3 re-records one node") ||
        !expect(first[3].hits == 0 && first[3].misses == visible,
                "step 4 re-records after the invalidation") ||
        !expect(first_sequences.size() == second_sequences.size(),
                "both runs emit one sequence per paint")) {
        return false;
    }
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (!expect(first[index].hits == second[index].hits &&
                        first[index].misses == second[index].misses &&
                        first[index].evictions == second[index].evictions &&
                        first[index].calls == second[index].calls,
                    "the hit/miss/eviction sequence is reproducible") ||
            !expect(same_calls(first_sequences[index], second_sequences[index]),
                    "the emitted sequence is reproducible")) {
            return false;
        }
    }
    return true;
}

// AppHost level: changing a class on the card is a style generation change. The label inherits the
// card's new color while its own dirty flags stay Clean, so a surviving record would replay the old
// color; begin_frame() must invalidate instead, once per generation.
bool test_app_host_style_generation_change() {
    Node root(1);
    Node& card = root.add_child(std::make_unique<Node>(2));
    card.set_class_name("card");
    Node& label = card.add_child(std::make_unique<Node>(3));
    label.set_element_kind("Text");
    PaintData text;
    text.kind = PaintKind::Text;
    text.text = "Hello";
    label.set_paint_data(std::move(text));

    // The label has no rule of its own: its color is inherited from the card. `.accent` therefore
    // changes the label's computed style while only the card is marked dirty.
    auto sheet = tessera::ui::style::compile_stylesheet(R"(
.card { width: 160px; height: 64px; padding: 8px; background-color: #1e293b; }
.accent { color: #00ff00; }
)");
    ManualClock clock;
    RuntimeLoop loop(clock);
    HostConfig config;
    config.width = 200.0f;
    config.height = 120.0f;
    AppHost host(root, std::move(sheet), loop, config);

    const std::size_t visible = count_nodes(root);
    RecordingSink sink;
    const HostFrame first_frame = host.begin_frame(0.0);
    const FramePaint first_paint = host.paint(sink, true);
    host.end_frame();
    const PaintCacheStats first = host.paint_cache_stats();
    const Call* first_text = find_text(sink.calls, "Hello");
    if (!expect(first_frame.style_rebuilt && first_paint.painted,
                "the first frame rebuilds the selector tree and paints") ||
        !expect(first.invalidations == 1,
                "the first generation starts from an invalidated cache") ||
        !expect(first.hits == 0 && first.misses == visible,
                "the first generation misses every node once") ||
        !expect(first_text != nullptr && same_color(first_text->color, rgba8(0, 0, 0)),
                "the label starts with the inherited (initial) color")) {
        return false;
    }

    // A new class on the card: the label's computed color changes through inheritance, but only the
    // card is marked dirty - the label stays Clean, which the cache key cannot see.
    card.set_class_name("card accent");
    const HostFrame restyled = host.begin_frame(1.0 / 60.0);
    sink.calls.clear();
    const FramePaint restyled_paint = host.paint(sink, true);
    host.end_frame();
    const PaintCacheStats generation = host.paint_cache_stats();
    const Call* restyled_text = find_text(sink.calls, "Hello");
    if (!expect(restyled.style_rebuilt && restyled_paint.painted,
                "the restyled frame rebuilds the selector tree and paints") ||
        !expect(generation.invalidations - first.invalidations == 1,
                "the generation change invalidates the cache once") ||
        !expect(generation.hits == first.hits, "the new generation starts with no hits") ||
        !expect(generation.misses - first.misses == visible,
                "every node re-records for the new generation") ||
        !expect(restyled_text != nullptr && same_color(restyled_text->color, rgba8(0, 255, 0)),
                "the label paints the new inherited color, not a stale replay")) {
        return false;
    }

    // The invalidation is one-shot: the next settled paint of the same generation replays.
    const HostFrame settled = host.begin_frame(1.0 / 60.0);
    sink.calls.clear();
    const FramePaint settled_paint = host.paint(sink, true);
    host.end_frame();
    const PaintCacheStats replay = host.paint_cache_stats();
    const Call* settled_text = find_text(sink.calls, "Hello");
    return expect(!settled.style_rebuilt, "a settled frame rebuilds nothing") &&
           expect(settled_paint.painted && settled_paint.stats.nodes == 0,
                  "the settled frame replays without resolving styles") &&
           expect(replay.hits - generation.hits == visible, "every node hits in the generation") &&
           expect(replay.misses == generation.misses, "the settled frame misses nothing") &&
           expect(replay.recorded_draws == generation.recorded_draws,
                  "the settled frame re-records nothing") &&
           expect(settled_text != nullptr && same_color(settled_text->color, rgba8(0, 255, 0)),
                  "the replayed text keeps the generation's color");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_settled_frame_replays_everything() && ok;
    ok = test_one_dirty_node_re_records() && ok;
    ok = test_invalidate_drops_every_record() && ok;
    ok = test_determinism() && ok;
    ok = test_app_host_style_generation_change() && ok;
    if (!ok) {
        std::cerr << "frame_paint_cache: failed\n";
        return 1;
    }
    std::cout << "frame_paint_cache: ok\n";
    return 0;
}
