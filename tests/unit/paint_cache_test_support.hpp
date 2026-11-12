// SPDX-License-Identifier: MIT
//
// Shared fixtures for the per-node paint cache tests (paint_cache_tests.cpp and
// paint_cache_bounds_tests.cpp): a scene that exercises every sink call kind, a recording sink that
// captures every call verbatim, a style stub that counts resolutions, and the test-only metrics
// source / PaintView overrides.

#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace paint_cache_test {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Mat3;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::rgba8;
using tessera::ui::Dirty;
using tessera::ui::LinearGradient;
using tessera::ui::LinearGradientStop;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::PaintWrap;
using tessera::ui::paint::GradientDraw;
using tessera::ui::paint::ImageDraw;
using tessera::ui::paint::kStubTextMetricsId;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintCache;
using tessera::ui::paint::PaintCacheConfig;
using tessera::ui::paint::PaintCacheStats;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::PaintTextMetricsSource;
using tessera::ui::paint::PaintView;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::paint::VectorDraw;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::FontMetrics;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextStyle;

inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_cache: " << message << "\n";
    }
    return condition;
}

inline ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

// Deterministic style stub that also counts resolutions, so a cache hit is observable as "no style
// was asked for".
class CountingStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }

    ComputedStyle compute(const Node& node) override {
        ++calls;
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

    std::size_t calls = 0;

private:
    std::map<NodeId, ComputedStyle> styles_;
};

inline void clear_all(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_all(*child);
    }
}

inline std::size_t count_nodes(const Node& node) {
    std::size_t total = 1;
    for (const auto& child : node.children()) {
        total += count_nodes(*child);
    }
    return total;
}

struct Call {
    enum class Kind {
        Rect,
        Rounded,
        Gradient,
        PushClip,
        PopClip,
        PushTransform,
        PopTransform,
        Text,
        Vector,
        Image,
    };

    Kind kind = Kind::Rect;
    Rect rect;
    Color color;
    float radius = 0.0f;
    RoundedClip clip;
    Mat3 transform;
    GradientDraw gradient;
    std::string text;
    float font_size = 0.0f;
    int font_weight = 0;
    std::optional<float> baseline;
    std::uint32_t raster_width = 0;
    std::uint32_t raster_height = 0;
    float scale = 0.0f;
    bool decode_original = false;
};

inline bool same_rect(const Rect& lhs, const Rect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

inline bool same_color(const Color& lhs, const Color& rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

inline bool same_matrix(const Mat3& lhs, const Mat3& rhs) {
    for (int index = 0; index < 9; ++index) {
        if (lhs.m[index] != rhs.m[index]) {
            return false;
        }
    }
    return true;
}

inline bool same_clip(const RoundedClip& lhs, const RoundedClip& rhs) {
    return same_rect(lhs.rect, rhs.rect) && lhs.top_left == rhs.top_left &&
           lhs.top_right == rhs.top_right && lhs.bottom_right == rhs.bottom_right &&
           lhs.bottom_left == rhs.bottom_left;
}

inline bool same_gradient(const GradientDraw& lhs, const GradientDraw& rhs) {
    if (!same_rect(lhs.rect, rhs.rect) || !same_clip(lhs.clip, rhs.clip) ||
        lhs.angle_degrees != rhs.angle_degrees || lhs.stop_count != rhs.stop_count) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.stop_count; ++index) {
        if (lhs.stops[index].offset != rhs.stops[index].offset ||
            !same_color(lhs.stops[index].color, rhs.stops[index].color)) {
            return false;
        }
    }
    return true;
}

// Byte-identical means every field of every call, in order.
inline bool same_call(const Call& lhs, const Call& rhs) {
    return lhs.kind == rhs.kind && same_rect(lhs.rect, rhs.rect) &&
           same_color(lhs.color, rhs.color) && lhs.radius == rhs.radius &&
           same_clip(lhs.clip, rhs.clip) && same_matrix(lhs.transform, rhs.transform) &&
           same_gradient(lhs.gradient, rhs.gradient) && lhs.text == rhs.text &&
           lhs.font_size == rhs.font_size && lhs.font_weight == rhs.font_weight &&
           lhs.baseline == rhs.baseline && lhs.raster_width == rhs.raster_width &&
           lhs.raster_height == rhs.raster_height && lhs.scale == rhs.scale &&
           lhs.decode_original == rhs.decode_original;
}

inline bool same_calls(const std::vector<Call>& lhs, const std::vector<Call>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (!same_call(lhs[index], rhs[index])) {
            return false;
        }
    }
    return true;
}

class RecordingSink final : public PaintSink {
public:
    explicit RecordingSink(bool transforms = true)
        : supports_transforms(transforms) {}

    void fill_rect(const Rect& rect, const Color& color) override {
        Call call;
        call.kind = Call::Kind::Rect;
        call.rect = rect;
        call.color = color;
        calls.push_back(std::move(call));
    }

    void fill_rounded_rect(const Rect& rect, const Color& color, float radius) override {
        Call call;
        call.kind = Call::Kind::Rounded;
        call.rect = rect;
        call.color = color;
        call.radius = radius;
        calls.push_back(std::move(call));
    }

    void fill_linear_gradient(const GradientDraw& draw) override {
        Call call;
        call.kind = Call::Kind::Gradient;
        call.gradient = draw;
        calls.push_back(std::move(call));
    }

    void push_clip(const RoundedClip& clip) override {
        Call call;
        call.kind = Call::Kind::PushClip;
        call.clip = clip;
        calls.push_back(std::move(call));
        ++clip_depth;
    }

    void pop_clip() override {
        Call call;
        call.kind = Call::Kind::PopClip;
        calls.push_back(std::move(call));
        --clip_depth;
    }

    [[nodiscard]] bool supports_transform() const override {
        return supports_transforms;
    }

    void push_transform(const Mat3& transform) override {
        Call call;
        call.kind = Call::Kind::PushTransform;
        call.transform = transform;
        calls.push_back(std::move(call));
    }

    void pop_transform() override {
        Call call;
        call.kind = Call::Kind::PopTransform;
        calls.push_back(std::move(call));
    }

    void draw_text(const TextDraw& run) override {
        Call call;
        call.kind = Call::Kind::Text;
        call.rect = run.rect;
        call.color = run.color;
        call.text.assign(run.text.data(), run.text.size());
        call.font_size = run.font_size;
        call.font_weight = run.font_weight;
        call.baseline = run.baseline;
        calls.push_back(std::move(call));
    }

    void draw_vector(const VectorDraw& draw) override {
        Call call;
        call.kind = Call::Kind::Vector;
        call.rect = draw.rect;
        call.color = draw.tint;
        call.text.assign(draw.asset.data(), draw.asset.size());
        call.raster_width = draw.raster_width;
        call.raster_height = draw.raster_height;
        call.scale = draw.scale;
        calls.push_back(std::move(call));
    }

    void draw_image(const ImageDraw& draw) override {
        Call call;
        call.kind = Call::Kind::Image;
        call.rect = draw.rect;
        call.color = draw.tint;
        call.text.assign(draw.asset.data(), draw.asset.size());
        call.raster_width = draw.raster_width;
        call.raster_height = draw.raster_height;
        call.scale = draw.scale;
        call.decode_original = draw.decode_original;
        calls.push_back(std::move(call));
    }

    std::vector<Call> calls;
    bool supports_transforms = true;
    int clip_depth = 0;
};

// One node of every emission kind: root rounded background + borders + shadow + overflow clip, a
// wrapped text run, a rotated vector asset, and an image + gradient node that itself clips a
// bordered child.
inline void build_rich_scene(Node& root, CountingStyles& styles) {
    root.set_bounds({0.0f, 0.0f, 240.0f, 180.0f});
    styles.set(1, styled({{"background-color", "#101820"},
                          {"border-top-width", "2px"},
                          {"border-top-style", "solid"},
                          {"border-top-color", "#ffffff"},
                          {"border-left-width", "2px"},
                          {"border-left-style", "solid"},
                          {"border-left-color", "#ffffff"},
                          {"border-top-left-radius", "8px"},
                          {"border-top-right-radius", "8px"},
                          {"border-bottom-right-radius", "8px"},
                          {"border-bottom-left-radius", "8px"},
                          {"box-shadow", "0 2px 8px #000000"},
                          {"overflow-x", "hidden"},
                          {"overflow-y", "hidden"},
                          {"font-size", "16px"},
                          {"color", "#ffffff"}}));

    auto text_owner = std::make_unique<Node>(2);
    text_owner->set_bounds({10.0f, 12.0f, 80.0f, 40.0f});
    PaintData text;
    text.text = "one two three four five six";
    text.wrap = PaintWrap::Wrap;
    text.max_lines = 2;
    text_owner->set_paint_data(std::move(text));
    root.add_child(std::move(text_owner));
    styles.set(2, styled({{"background-color", "#ff0000"},
                          {"padding-left", "4px"},
                          {"padding-top", "3px"},
                          {"font-size", "16px"},
                          {"color", "#ffffff"}}));

    auto vector_owner = std::make_unique<Node>(3);
    vector_owner->set_bounds({100.0f, 12.0f, 60.0f, 60.0f});
    vector_owner->set_transform(Mat3::rotation(0.35f));
    PaintData vector;
    vector.kind = PaintKind::Vector;
    vector.vector.asset = "icon";
    vector.vector.tint = rgba8(255, 255, 255);
    vector_owner->set_paint_data(std::move(vector));
    root.add_child(std::move(vector_owner));
    styles.set(3, styled({{"background-color", "#00ff00"}}));

    auto clip_owner = std::make_unique<Node>(4);
    clip_owner->set_bounds({10.0f, 70.0f, 200.0f, 80.0f});
    PaintData image;
    image.kind = PaintKind::Image;
    image.image.asset = "photo";
    image.image.raster_width = 32;
    image.gradient = LinearGradient{};
    image.gradient->angle_degrees = 90.0f;
    image.gradient->stop_count = 2;
    image.gradient->stops[0] = LinearGradientStop{0.0f, rgba8(0, 0, 255)};
    image.gradient->stops[1] = LinearGradientStop{1.0f, rgba8(255, 255, 0)};
    clip_owner->set_paint_data(std::move(image));
    Node& clip_node = root.add_child(std::move(clip_owner));
    styles.set(4, styled({{"background-color", "#0000ff"},
                          {"overflow-x", "scroll"},
                          {"overflow-y", "scroll"}}));

    auto border_owner = std::make_unique<Node>(5);
    border_owner->set_bounds({8.0f, 8.0f, 40.0f, 24.0f});
    clip_node.add_child(std::move(border_owner));
    styles.set(5, styled({{"background-color", "#ffffff"},
                          {"border-top-width", "3px"},
                          {"border-top-style", "solid"},
                          {"border-top-color", "#ff00ff"}}));
}

// Test-only metrics source with a distinct id; geometry is the stub layout, so a changed id alone
// must miss the cache without changing the expected draws.
class FixedMetrics final : public PaintTextMetricsSource {
public:
    explicit FixedMetrics(std::uint32_t source_id)
        : id_(source_id) {}

    [[nodiscard]] std::uint32_t id() const override {
        return id_;
    }
    [[nodiscard]] FontMetrics metrics(const TextStyle& style) const override {
        return tessera::ui::paint::StubPaintTextMetrics{}.metrics(style);
    }
    [[nodiscard]] TextLayout layout(std::string_view text, const TextStyle& style,
                                    const TextConstraints& constraints) const override {
        return tessera::ui::paint::StubPaintTextMetrics{}.layout(text, style, constraints);
    }
    [[nodiscard]] float advance(std::string_view text, const TextStyle& style) const override {
        return tessera::ui::paint::StubPaintTextMetrics{}.advance(text, style);
    }

private:
    std::uint32_t id_ = 0;
};

// A paint-only override that recolors every node, used to prove a view bypasses the cache instead
// of replaying the recorded (stale) draws.
class RecolorView final : public PaintView {
public:
    [[nodiscard]] std::optional<Mat3> local_transform(NodeId) const override {
        return std::nullopt;
    }
    void overlay_style(NodeId, ComputedStyle& style) const override {
        style.set("background-color", "#0000ff");
    }
};

// Defined in paint_cache_tests.cpp (replay, invalidation matrix) and paint_cache_bounds_tests.cpp
// (budgets, LRU order, determinism).
bool test_byte_identical_replay();
bool test_clean_subtree_is_not_re_recorded();
bool test_dirty_node_re_records_changed_output();
bool test_bounds_change_misses();
bool test_transform_change_misses();
bool test_inherited_clip_change_misses_child();
bool test_metrics_source_and_view_bypass();
bool test_entry_budget_and_byte_budget();
bool test_lru_order();
bool test_determinism();

} // namespace paint_cache_test
