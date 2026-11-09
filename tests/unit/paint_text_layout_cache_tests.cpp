// SPDX-License-Identifier: MIT
//
// Bounded paint text-layout cache: deterministic key isolation, LRU eviction by both budgets,
// exact byte accounting, cached-vs-fresh line-box equality, invalidate/no-cache behavior, paint
// pass integration, and a `--bench` measurement for a 200-word paragraph at a 300 px wrap width.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/paint/text_layout_cache.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::kStubTextMetricsId;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::PaintTextMetricsSource;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StubPaintTextMetrics;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::paint::TextLayoutCache;
using tessera::ui::paint::TextLayoutCacheConfig;
using tessera::ui::paint::TextLayoutCacheStats;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::FontMetrics;
using tessera::ui::text::StubFontMetricsProvider;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextLine;
using tessera::ui::text::TextStyle;

struct Request {
    std::string text = "one two three four five six seven eight nine ten";
    TextStyle style{};
    float width = 80.0f;
    std::uint32_t max_lines = 0;
    std::uint32_t metrics_id = kStubTextMetricsId;
    bool wrap = true;
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_layout_cache: " << message << "\n";
    }
    return condition;
}

FontMetrics stub_metrics(const TextStyle& style) {
    return StubFontMetricsProvider{}.metrics(style);
}

// Test-only metrics source with a distinct id. Its line boxes are the stub layout so a changed id
// alone misses the cache without changing the expected geometry.
class FixedMetrics final : public PaintTextMetricsSource {
public:
    explicit FixedMetrics(std::uint32_t source_id)
        : id_(source_id) {}

    [[nodiscard]] std::uint32_t id() const override {
        return id_;
    }
    [[nodiscard]] FontMetrics metrics(const TextStyle& style) const override {
        return StubFontMetricsProvider{}.metrics(style);
    }
    [[nodiscard]] TextLayout layout(std::string_view text, const TextStyle& style,
                                    const TextConstraints& constraints) const override {
        return StubPaintTextMetrics{}.layout(text, style, constraints);
    }

private:
    std::uint32_t id_ = 0;
};

const TextLayout& get(TextLayoutCache& cache, const Request& request) {
    TextStyle style = request.style;
    style.nowrap = !request.wrap;
    TextConstraints constraints;
    constraints.wrap = request.wrap;
    constraints.max_width = request.width;
    StubPaintTextMetrics stub;
    FixedMetrics fake(request.metrics_id);
    const PaintTextMetricsSource& source = request.metrics_id == kStubTextMetricsId
                                               ? static_cast<const PaintTextMetricsSource&>(stub)
                                               : static_cast<const PaintTextMetricsSource&>(fake);
    return cache.get(request.text, style, constraints, source, request.max_lines);
}

TextLayout fresh(const Request& request) {
    TextStyle style = request.style;
    style.nowrap = !request.wrap;
    TextConstraints constraints;
    constraints.wrap = request.wrap;
    constraints.max_width = request.width;
    TextLayout layout =
        tessera::ui::text::layout_text(request.text, style, stub_metrics(style), constraints);
    if (request.max_lines > 0 && layout.lines.size() > request.max_lines) {
        layout.lines.resize(request.max_lines);
    }
    return layout;
}

bool same_line(const TextLine& a, const TextLine& b) {
    return a.width == b.width && a.height == b.height && a.baseline == b.baseline &&
           a.begin == b.begin && a.end == b.end;
}

bool same_lines(const TextLayout& a, const TextLayout& b) {
    if (a.lines.size() != b.lines.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.lines.size(); ++index) {
        if (!same_line(a.lines[index], b.lines[index])) {
            return false;
        }
    }
    return a.width == b.width && a.height == b.height && a.baseline == b.baseline;
}

bool test_hits_and_misses() {
    TextLayoutCache cache;
    const Request request;
    const TextLayout& first = get(cache, request);
    TextLayoutCacheStats stats = cache.stats();
    bool ok = expect(stats.misses == 1 && stats.hits == 0 && stats.entries == 1,
                     "first get is a miss and inserts") &&
              expect(stats.bytes > 0, "byte accounting is non-zero");
    const TextLayout& second = get(cache, request);
    stats = cache.stats();
    ok = expect(stats.hits == 1 && stats.misses == 1 && stats.entries == 1,
                "second identical get is a hit") &&
         expect(&first == &second, "hit returns the same stable view") &&
         expect(same_lines(first, second), "hit returns identical line boxes") && ok;
    return ok;
}

bool test_key_isolation() {
    TextLayoutCache cache;
    Request base;
    get(cache, base);
    const TextLayoutCacheStats initial = cache.stats();

    std::vector<Request> variants;
    auto add = [&](auto mutate) {
        Request variant = base;
        mutate(variant);
        variants.push_back(variant);
    };
    add([](Request& r) { r.text += "!"; });
    add([](Request& r) { r.width += 1.0f; });
    add([](Request& r) { r.style.font_size += 1.0f; });
    add([](Request& r) { r.style.font_weight += 100; });
    add([](Request& r) { r.style.italic = true; });
    add([](Request& r) { r.style.letter_spacing += 0.5f; });
    add([](Request& r) { r.style.line_height += 1.0f; });
    add([](Request& r) { r.wrap = false; });
    add([](Request& r) { r.max_lines = 2; });
    add([](Request& r) { r.metrics_id = 7; });

    bool ok = true;
    std::uint64_t misses = initial.misses;
    for (const Request& variant : variants) {
        (void)get(cache, variant);
        const TextLayoutCacheStats stats = cache.stats();
        ok = expect(stats.misses == misses + 1, "every changed key component misses") && ok;
        misses = stats.misses;
    }
    ok = expect(cache.stats().hits == 0, "key variants did not hit") && ok;

    // Exact bit-level float keys: -0.0 vs +0.0 are different widths.
    Request negative_zero = base;
    negative_zero.width = -0.0f;
    Request positive_zero = base;
    positive_zero.width = 0.0f;
    const std::uint64_t before = cache.stats().misses;
    (void)get(cache, negative_zero);
    const std::uint64_t after_negative = cache.stats().misses;
    (void)get(cache, positive_zero);
    ok = expect(after_negative == before + 1 && cache.stats().misses == after_negative + 1,
                "float key fields compare by exact bits") &&
         ok;
    return ok;
}

bool test_lru_eviction_by_entries() {
    TextLayoutCache cache(TextLayoutCacheConfig{2, 1024 * 1024});
    Request a;
    a.text = "alpha beta gamma";
    Request b;
    b.text = "delta epsilon zeta";
    Request c;
    c.text = "eta theta iota";
    (void)get(cache, a);
    (void)get(cache, b);
    (void)get(cache, a); // touch A so B is least-recently used
    (void)get(cache, c); // evicts B
    TextLayoutCacheStats stats = cache.stats();
    bool ok = expect(stats.entries == 2 && stats.evictions == 1,
                     "entry budget evicts exactly one LRU entry");
    const std::uint64_t hits_before = stats.hits;
    (void)get(cache, b);
    stats = cache.stats();
    ok = expect(stats.misses == 4 && stats.hits == hits_before,
                "evicted entry misses again without a stale hit") &&
         ok;
    return ok;
}

bool test_lru_eviction_by_bytes() {
    TextLayoutCache probe(TextLayoutCacheConfig{64, 1024 * 1024});
    Request large;
    large.text = "one two three four five six seven eight nine ten eleven twelve";
    (void)get(probe, large);
    const std::size_t entry_bytes = probe.stats().bytes;
    if (!expect(entry_bytes > 0, "probe entry has a byte cost")) {
        return false;
    }

    TextLayoutCache cache(TextLayoutCacheConfig{64, entry_bytes});
    Request small;
    small.text = "a b";
    (void)get(cache, large);
    (void)get(cache, small);
    TextLayoutCacheStats stats = cache.stats();
    bool ok =
        expect(stats.evictions == 1 && stats.entries == 1, "byte budget evicts the LRU entry") &&
        expect(stats.bytes <= entry_bytes, "byte accounting stays within the budget");
    const std::uint64_t hits_before = stats.hits;
    (void)get(cache, small);
    ok = expect(cache.stats().hits == hits_before + 1, "remaining byte-budget entry hits") && ok;
    return ok;
}

bool test_cached_equals_fresh() {
    TextLayoutCache cache;
    const auto make = [](std::string text, float width, std::uint32_t max_lines = 0,
                         bool wrap = true, bool italic = true, int weight = 600,
                         float spacing = 0.5f) {
        Request request;
        request.text = std::move(text);
        request.width = width;
        request.max_lines = max_lines;
        request.wrap = wrap;
        request.style.font_size = 16.0f;
        request.style.font_weight = weight;
        request.style.italic = italic;
        request.style.letter_spacing = spacing;
        return request;
    };
    const std::array<Request, 6> requests = {
        make("one two three four five six seven eight nine ten", 80.0f),
        make("supercalifragilisticexpialidocious word wrap", 60.0f),
        make("line one\nline two\nline three", 120.0f),
        make("one two three four five six", 70.0f, 2),
        make("no wrapping here", 40.0f, 0, false),
        make("weighted and italic", 90.0f),
    };
    bool ok = true;
    for (const Request& request : requests) {
        const TextLayout expected = fresh(request);
        const TextLayout& cached = get(cache, request);
        ok = expect(same_lines(cached, expected), "cached layout equals fresh layout") && ok;
        const TextLayout& again = get(cache, request);
        ok = expect(same_lines(again, expected), "cached hit equals fresh layout") && ok;
    }
    ok = expect(cache.stats().hits == requests.size(), "fixture set hits cached entries") && ok;
    return ok;
}

bool test_disable_and_invalidate() {
    TextLayoutCache cache;
    const Request request;
    (void)get(cache, request);
    const auto entries = cache.stats().entries;
    cache.set_enabled(false);
    bool ok = expect(!cache.enabled(), "cache reports disabled") &&
              expect(entries == 1, "disabling keeps existing entries");
    const TextLayout& disabled = get(cache, request);
    ok =
        expect(same_lines(disabled, fresh(request)), "disabled cache still returns fresh layout") &&
        expect(cache.stats().entries == 1, "disabled cache does not insert or evict") && ok;

    cache.set_enabled(true);
    const TextLayoutCacheStats before = cache.stats();
    (void)get(cache, request);
    ok =
        expect(cache.stats().hits == before.hits + 1, "re-enabled cache hits existing entry") && ok;

    cache.invalidate();
    const TextLayoutCacheStats after_invalidate = cache.stats();
    ok = expect(after_invalidate.entries == 0 && after_invalidate.bytes == 0 &&
                    after_invalidate.invalidations == 1,
                "invalidate drops entries and counts the generation change") &&
         ok;
    (void)get(cache, request);
    ok = expect(cache.stats().misses == after_invalidate.misses + 1,
                "invalidate forces the next get to miss") &&
         ok;
    return ok;
}

struct RecordedText {
    tessera::rendering::rhi::Rect rect;
    std::string text;
    std::optional<float> baseline;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const tessera::rendering::rhi::Rect&,
                   const tessera::rendering::rhi::Color&) override {}
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&, float) override {}
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        texts.push_back(RecordedText{run.rect, std::string(run.text), run.baseline});
    }
    std::vector<RecordedText> texts;
};

class StubStyles final : public StyleProvider {
public:
    ComputedStyle compute(const Node&) override {
        ComputedStyle style;
        style.set("font-size", "16px");
        return style;
    }
};

bool test_paint_integration() {
    Node root(1);
    root.set_bounds({0, 0, 80, 120});
    tessera::ui::PaintData data;
    data.text = "one two three four five six seven eight nine ten";
    data.wrap = tessera::ui::PaintWrap::Wrap;
    data.max_lines = 3;
    root.set_paint_data(std::move(data));

    StubStyles styles;
    TextLayoutCache cache;
    PaintContext context;
    context.text_layout_cache = &cache;

    RecordingSink uncached;
    paint_scene(root, styles, uncached);
    RecordingSink cached_first;
    paint_scene(root, styles, cached_first, context);
    RecordingSink cached_second;
    paint_scene(root, styles, cached_second, context);

    bool ok = expect(uncached.texts.size() == 3 && cached_first.texts.size() == 3,
                     "paint emits max_lines lines with and without the cache");
    ok = expect(cache.stats().hits == 1 && cache.stats().misses == 1,
                "second paint hits the text layout cache") &&
         ok;
    for (std::size_t index = 0; index < uncached.texts.size() && index < cached_first.texts.size();
         ++index) {
        ok = expect(uncached.texts[index].text == cached_first.texts[index].text &&
                        uncached.texts[index].rect.x == cached_first.texts[index].rect.x &&
                        uncached.texts[index].rect.y == cached_first.texts[index].rect.y &&
                        uncached.texts[index].baseline == cached_first.texts[index].baseline,
                    "cached paint emits identical line draws") &&
             ok;
    }
    return ok;
}

bool test_determinism() {
    TextLayoutCache first;
    TextLayoutCache second;
    const Request request;
    const TextLayout& a = get(first, request);
    const TextLayout& b = get(second, request);
    const TextLayout expected = fresh(request);
    return expect(same_lines(a, expected) && same_lines(b, expected),
                  "two cache instances produce the same line boxes") &&
           expect(first.stats().bytes == second.stats().bytes,
                  "two cache instances account identical bytes");
}

void benchmark() {
    std::string text;
    for (int word = 0; word < 200; ++word) {
        if (word > 0) {
            text += ' ';
        }
        text += "word" + std::to_string(word);
    }
    TextStyle style;
    style.font_size = 16.0f;
    const FontMetrics metrics = stub_metrics(style);
    TextConstraints constraints;
    constraints.wrap = true;
    constraints.max_width = 300.0f;

    constexpr int kIterations = 200;
    std::size_t lines = 0;
    const auto time = [](auto&& call) {
        const auto started = std::chrono::steady_clock::now();
        call();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        return static_cast<double>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
               1000.0;
    };

    double fresh_us = 0.0;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        TextLayout layout;
        fresh_us += time(
            [&] { layout = tessera::ui::text::layout_text(text, style, metrics, constraints); });
        lines = layout.lines.size();
    }

    double miss_us = 0.0;
    TextLayoutCache cache(TextLayoutCacheConfig{1, 1024 * 1024});
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        cache.invalidate();
        const auto started = std::chrono::steady_clock::now();
        (void)cache.get(text, style, constraints, StubPaintTextMetrics{}, 0);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        miss_us += static_cast<double>(
                       std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
                   1000.0;
    }

    (void)cache.get(text, style, constraints, StubPaintTextMetrics{}, 0);
    double hit_us = 0.0;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        (void)cache.get(text, style, constraints, StubPaintTextMetrics{}, 0);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        hit_us += static_cast<double>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
                  1000.0;
    }

    std::cout << "text_layout_cache bench: 200 words, 300px -> " << lines
              << " lines, fresh=" << fresh_us / kIterations
              << " us, miss(insert)=" << miss_us / kIterations
              << " us, hit=" << hit_us / kIterations << " us (Debug)\n";
    std::cout << "text_layout_cache bench stats: entries=" << cache.stats().entries
              << " bytes=" << cache.stats().bytes << " hits=" << cache.stats().hits
              << " misses=" << cache.stats().misses << "\n";
}

} // namespace

int main(int argc, char** argv) {
    bool ok = true;
    ok = test_hits_and_misses() && ok;
    ok = test_key_isolation() && ok;
    ok = test_lru_eviction_by_entries() && ok;
    ok = test_lru_eviction_by_bytes() && ok;
    ok = test_cached_equals_fresh() && ok;
    ok = test_disable_and_invalidate() && ok;
    ok = test_paint_integration() && ok;
    ok = test_determinism() && ok;

    const bool bench = argc > 1 && std::string_view(argv[1]) == "--bench";
    if (bench) {
        benchmark();
    }
    if (!ok) {
        std::cerr << "text_layout_cache: failed\n";
        return 1;
    }
    std::cout << "text_layout_cache: ok\n";
    return 0;
}
