// SPDX-License-Identifier: MIT
//
// IME preedit rendering (task-72): descriptor <-> CompositionStart/Update/commit/cancel, underline
// range only, preedit run at the caret with an empty buffer, z-order (selection < glyphs < preedit
// < caret), device-scale thickness, damage/clip replay, opacity folding and determinism. Stub
// metrics keep the geometry deterministic; no new PNG golden (a RecordingSink byte-exact op record
// is the assertion mechanism, and the CPU text path needs a real font for pixels).

#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/text/edit_controller.hpp"
#include "tessera/ui/text/font_metrics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::Scissor;
using tessera::ui::Event;
using tessera::ui::EventType;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::TextDecoration;
using tessera::ui::paint::ClippedPaintSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintTextMetricsSource;
using tessera::ui::paint::StubPaintTextMetrics;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::EditController;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_text_preedit: " << message << "\n";
    }
    return condition;
}

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

enum class EventKind { Fill, Text };

struct PaintEvent {
    EventKind kind = EventKind::Fill;
    Rect rect;
    Color color;
    std::string text;

    bool operator==(const PaintEvent& other) const {
        return kind == other.kind && rect.x == other.rect.x && rect.y == other.rect.y &&
               rect.width == other.rect.width && rect.height == other.rect.height &&
               color.r == other.color.r && color.g == other.color.g && color.b == other.color.b &&
               color.a == other.color.a && text == other.text;
    }
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        events.push_back(PaintEvent{EventKind::Fill, rect, color, {}});
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        fill_rect(rect, color);
    }
    void draw_text(const TextDraw& run) override {
        events.push_back(PaintEvent{EventKind::Text, run.rect, run.color, std::string(run.text)});
        ++texts;
    }

    std::size_t count(EventKind kind) const {
        std::size_t result = 0;
        for (const PaintEvent& event : events) {
            result += event.kind == kind ? 1 : 0;
        }
        return result;
    }
    const PaintEvent* last(EventKind kind) const {
        const PaintEvent* found = nullptr;
        for (const PaintEvent& event : events) {
            if (event.kind == kind) {
                found = &event;
            }
        }
        return found;
    }

    std::vector<PaintEvent> events;
    std::size_t texts = 0;
};

constexpr float kFontSize = 20.0f;
constexpr std::uint32_t kNodeId = 1;

const Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};

float advance_of(std::string_view text) {
    StubPaintTextMetrics metrics;
    TextStyle style;
    style.font_size = kFontSize;
    return metrics.advance(text, style);
}

tessera::ui::text::FontMetrics stub_metrics() {
    tessera::ui::text::StubFontMetricsProvider provider;
    TextStyle style;
    style.font_size = kFontSize;
    return provider.metrics(style);
}

TextDecoration decoration() {
    TextDecoration value;
    value.caret_visible = false;
    value.blink_phase = 0.0f;
    return value;
}

void run_paint(const std::string& text, const TextDecoration& value, PaintSink& sink,
               float scale = 1.0f, std::optional<float> opacity = std::nullopt) {
    Node node(kNodeId);
    node.set_bounds({0.0f, 0.0f, 200.0f, 60.0f});
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = text;
    data.text_style.font_size = kFontSize;
    data.text_style.color = kWhite;
    data.decoration = value;
    node.set_paint_data(std::move(data));

    StubStyles styles;
    if (opacity.has_value()) {
        styles.set(kNodeId, styled({{"opacity", "0.5"}}));
    }
    PaintContext context;
    context.device_scale = scale;
    paint_scene(node, styles, sink, context);
}

Event payload_event(EventType type, std::string_view text) {
    Event event;
    event.type = type;
    const std::size_t size = std::min<std::size_t>(text.size(), event.text.size());
    std::copy_n(text.data(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    return event;
}

bool feed(EditController& controller, Event event) {
    return controller.handle_event(event);
}

bool descriptor_lifecycle() {
    EditController controller;
    if (!expect(!controller.preedit_range().active, "no composition initially") ||
        !expect(feed(controller, payload_event(EventType::CompositionStart, {})),
                "composition start consumed")) {
        return false;
    }
    const auto started = controller.preedit_range();
    if (!expect(started.active, "composition start activates the descriptor") ||
        !expect(started.begin == 0 && started.end == 0, "empty composition range is the caret") ||
        !expect(started.text.empty(), "empty preedit initially")) {
        return false;
    }
    if (!expect(feed(controller, payload_event(EventType::CompositionUpdate, "にほ")),
                "composition update consumed")) {
        return false;
    }
    const auto updated = controller.preedit_range();
    if (!expect(updated.active && updated.text == "にほ", "preedit bytes exposed") ||
        !expect(updated.begin == 0 && updated.end == 0, "insertion range stays collapsed") ||
        !expect(controller.buffer().text().empty(), "preedit stays out of the committed buffer")) {
        return false;
    }
    // Commit: the descriptor deactivates and exactly one TextChange-visible commit happens.
    if (!expect(feed(controller, payload_event(EventType::EditText, "日本")), "commit consumed")) {
        return false;
    }
    const auto committed = controller.preedit_range();
    if (!expect(!committed.active, "commit deactivates the descriptor") ||
        !expect(committed.text.empty(), "commit clears the preedit bytes") ||
        !expect(controller.buffer().text() == "日本", "commit applied to the buffer") ||
        !expect(controller.committed_edit_count() == 1, "one committed edit for the commit")) {
        return false;
    }
    // Cancel: CompositionEnd without EditText restores the pre-composition state and emits nothing.
    EditController cancelled;
    feed(cancelled, payload_event(EventType::CompositionStart, {}));
    feed(cancelled, payload_event(EventType::CompositionUpdate, "か"));
    if (!expect(feed(cancelled, payload_event(EventType::CompositionEnd, {})),
                "composition end consumed")) {
        return false;
    }
    return expect(!cancelled.preedit_range().active, "cancel deactivates the descriptor") &&
           expect(cancelled.buffer().text().empty(), "cancel keeps the buffer unchanged") &&
           expect(cancelled.committed_edit_count() == 0, "cancel emits no committed edit");
}

bool underline_range_only() {
    TextDecoration value = decoration();
    value.composition_active = true;
    value.preedit_begin = 1;
    value.preedit_end = 3;
    value.preedit_underline = true;

    RecordingSink sink;
    run_paint("abcd", value, sink);
    if (!expect(sink.count(EventKind::Text) == 1, "one glyph run for the committed text") ||
        !expect(sink.events[0].text == "abcd", "glyph run is the plain text") ||
        !expect(sink.count(EventKind::Fill) == 1, "exactly one underline fill")) {
        return false;
    }
    const PaintEvent* underline = sink.last(EventKind::Fill);
    const tessera::ui::text::FontMetrics metrics = stub_metrics();
    const float expected_x = std::round(advance_of("a"));
    const float expected_width = std::round(advance_of("bc"));
    const float expected_y = std::round(metrics.ascent + metrics.descent + metrics.line_gap - 1.0f);
    return expect(std::abs(underline->rect.x - expected_x) < 0.5f,
                  "underline starts at the range") &&
           expect(std::abs(underline->rect.width - expected_width) < 0.5f,
                  "underline covers only the range") &&
           expect(std::abs(underline->rect.y - expected_y) < 0.5f,
                  "underline sits at the baseline") &&
           expect(std::abs(underline->rect.height - 1.0f) < 0.01f, "1 px underline at scale 1") &&
           expect(underline->color.r == kWhite.r && underline->color.g == kWhite.g &&
                      underline->color.b == kWhite.b && underline->color.a == kWhite.a,
                  "underline uses the text color");
}

bool preedit_run_at_empty_buffer() {
    TextDecoration value = decoration();
    value.composition_active = true;
    value.preedit_begin = 0;
    value.preedit_end = 0;
    value.preedit_text = "にほ";
    value.preedit_underline = true;

    RecordingSink sink;
    run_paint("", value, sink);
    if (!expect(sink.count(EventKind::Text) == 1, "preedit run drawn for an empty buffer") ||
        !expect(sink.events[0].text == "にほ", "preedit bytes drawn") ||
        !expect(sink.count(EventKind::Fill) == 1, "preedit underline drawn")) {
        return false;
    }
    const tessera::ui::text::FontMetrics metrics = stub_metrics();
    const PaintEvent& run = sink.events[0];
    const PaintEvent* underline = sink.last(EventKind::Fill);
    return expect(run.rect.x == 0.0f && run.rect.y == 0.0f, "preedit starts at the caret") &&
           expect(std::abs(run.rect.width - std::round(advance_of("にほ"))) < 0.5f,
                  "preedit run width from the metrics source") &&
           expect(std::abs(run.rect.height -
                           (metrics.ascent + metrics.descent + metrics.line_gap)) < 0.5f,
                  "preedit run uses the line height") &&
           expect(std::abs(underline->rect.width - run.rect.width) < 0.5f,
                  "underline spans the preedit run") &&
           expect(underline->rect.y > run.rect.y, "underline below the preedit top");
}

bool scale_two_thickness() {
    TextDecoration value = decoration();
    value.composition_active = true;
    value.preedit_text = "x";
    RecordingSink sink;
    run_paint("", value, sink, 2.0f);
    const PaintEvent* underline = sink.last(EventKind::Fill);
    return expect(underline != nullptr, "scale 2 underline emitted") &&
           expect(std::abs(underline->rect.height - 2.0f) < 0.01f,
                  "2 px underline at device scale >= 2");
}

bool commit_and_cancel_remove_visuals() {
    // Committed state: decoration deactivated, committed text painted normally.
    TextDecoration inactive = decoration();
    RecordingSink committed;
    run_paint("日本", inactive, committed);
    if (!expect(committed.count(EventKind::Text) == 1, "committed text still paints") ||
        !expect(committed.count(EventKind::Fill) == 0, "no underline after commit")) {
        return false;
    }
    // Cancelled composition: active=false and the buffer text unchanged -> same as above.
    RecordingSink cancelled;
    run_paint("あ", inactive, cancelled);
    return expect(cancelled.count(EventKind::Fill) == 0, "no underline after cancel");
}

bool z_order() {
    TextDecoration value = decoration();
    value.caret_visible = true;
    value.blink_phase = 1.0f;
    value.caret_byte_offset = 2;
    value.selection_begin = 1;
    value.selection_end = 3;
    value.composition_active = true;
    value.preedit_begin = 2;
    value.preedit_end = 2;
    value.preedit_text = "X";

    RecordingSink sink;
    run_paint("abcd", value, sink);
    // Selection fill, glyph run, preedit run, preedit underline, caret (in this order).
    if (!expect(sink.events.size() == 5, "five decoration ops expected")) {
        return false;
    }
    return expect(sink.events[0].kind == EventKind::Fill, "selection fill first") &&
           expect(sink.events[1].kind == EventKind::Text && sink.events[1].text == "abcd",
                  "committed glyph run second") &&
           expect(sink.events[2].kind == EventKind::Text && sink.events[2].text == "X",
                  "preedit run above the glyphs") &&
           expect(sink.events[3].kind == EventKind::Fill, "preedit underline above selection") &&
           expect(sink.events[4].kind == EventKind::Fill, "caret last / on top");
}

bool damage_replay_clips_preedit() {
    TextDecoration value = decoration();
    value.composition_active = true;
    value.preedit_text = "にほ";

    // A region to the right of the preedit: nothing is painted.
    RecordingSink outside;
    const std::vector<Scissor> right{Scissor{150, 0, 40, 60}};
    ClippedPaintSink no_hit(outside, right);
    run_paint("", value, no_hit);
    if (!expect(outside.events.empty(), "preedit outside the damage region emits nothing")) {
        return false;
    }
    // A region covering the preedit: the run replays once, the underline is clipped to the region.
    RecordingSink covered;
    const std::vector<Scissor> left{Scissor{0, 0, 3, 60}};
    ClippedPaintSink hit(covered, left);
    run_paint("", value, hit);
    if (!expect(covered.count(EventKind::Text) == 1, "preedit run replayed once") ||
        !expect(covered.count(EventKind::Fill) == 1, "underline replayed once")) {
        return false;
    }
    const PaintEvent* underline = covered.last(EventKind::Fill);
    return expect(underline->rect.width <= 3.0f, "underline clipped to the damage region") &&
           expect(underline->rect.x == 0.0f, "underline keeps its origin");
}

bool opacity_folds_into_preedit() {
    TextDecoration value = decoration();
    value.composition_active = true;
    value.preedit_text = "x";
    RecordingSink sink;
    run_paint("", value, sink, 1.0f, 0.5f);
    if (!expect(sink.count(EventKind::Text) == 1 && sink.count(EventKind::Fill) == 1,
                "preedit run and underline emitted")) {
        return false;
    }
    return expect(std::abs(sink.events[0].color.a - 0.5f) < 0.01f,
                  "preedit run alpha folded with node opacity") &&
           expect(std::abs(sink.last(EventKind::Fill)->color.a - 0.5f) < 0.01f,
                  "underline alpha folded with node opacity");
}

bool inactive_empty_text_is_still_noop() {
    RecordingSink sink;
    run_paint("", decoration(), sink);
    return expect(sink.events.empty(), "empty text without composition still paints nothing");
}

bool determinism() {
    TextDecoration value = decoration();
    value.caret_visible = true;
    value.blink_phase = 1.0f;
    value.caret_byte_offset = 2;
    value.selection_begin = 1;
    value.selection_end = 3;
    value.composition_active = true;
    value.preedit_begin = 2;
    value.preedit_end = 2;
    value.preedit_text = "にほ";
    RecordingSink first;
    RecordingSink second;
    run_paint("abcd", value, first);
    run_paint("abcd", value, second);
    return expect(first.events == second.events, "the same paint gives identical op records");
}

} // namespace

int main() {
    if (!descriptor_lifecycle()) {
        return 1;
    }
    if (!underline_range_only()) {
        return 2;
    }
    if (!preedit_run_at_empty_buffer()) {
        return 3;
    }
    if (!scale_two_thickness()) {
        return 4;
    }
    if (!commit_and_cancel_remove_visuals()) {
        return 5;
    }
    if (!z_order()) {
        return 6;
    }
    if (!damage_replay_clips_preedit()) {
        return 7;
    }
    if (!opacity_folds_into_preedit()) {
        return 8;
    }
    if (!inactive_empty_text_is_still_noop()) {
        return 9;
    }
    if (!determinism()) {
        return 10;
    }

    std::cout << "paint_text_preedit: ok\n";
    return 0;
}
