// SPDX-License-Identifier: MIT
//
// CSS `text-decoration` and `vertical-align` (docs/design/text-decoration.md): the value grammar,
// the registry/shorthand expansion and `@supports`, the computed-value mapping, and the painted
// rects and baseline shift for plain, wrapped and spanned text. Deterministic stub metrics only.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/supports.hpp"
#include "tessera/ui/style/text_decoration.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintSpan;
using tessera::ui::PaintWrap;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::PropertyRegistry;
namespace style = tessera::ui::style;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "text_decoration: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.01f;
}

// --- 1. grammar ----------------------------------------------------------------------------

bool grammar() {
    using style::parse_text_decoration_line;
    bool ok =
        expect(parse_text_decoration_line("none") == 0, "none is the empty set") &&
        expect(parse_text_decoration_line("underline") == style::kDecorationUnderline,
               "underline") &&
        expect(parse_text_decoration_line(" Underline  LINE-THROUGH ") ==
                   (style::kDecorationUnderline | style::kDecorationLineThrough),
               "keywords combine, any case and spacing") &&
        expect(parse_text_decoration_line("overline underline line-through") == 7, "all three") &&
        expect(!parse_text_decoration_line("underline underline"), "repeat is invalid") &&
        expect(!parse_text_decoration_line("none underline"), "none cannot combine") &&
        expect(!parse_text_decoration_line("blink"), "blink is not drawable") &&
        expect(!parse_text_decoration_line(""), "empty is invalid");
    ok = expect(style::is_supported_text_decoration_style("solid"), "solid") &&
         expect(!style::is_supported_text_decoration_style("wavy"), "wavy rejected") && ok;
    ok = expect(near(style::text_decoration_thickness("2px", 16.0f), 2.0f), "px thickness") &&
         expect(near(style::text_decoration_thickness("0.5em", 20.0f), 10.0f), "em thickness") &&
         expect(style::text_decoration_thickness("auto", 16.0f) == 0.0f, "auto is 0") &&
         expect(!style::is_supported_text_decoration_thickness("-1px"), "negative rejected") &&
         expect(!style::is_supported_text_decoration_thickness("10%"), "percent rejected") && ok;
    ok = expect(near(style::vertical_align_shift("baseline", 16.0f), 0.0f), "baseline") &&
         expect(near(style::vertical_align_shift("super", 10.0f), 3.3f), "super raises 0.33em") &&
         expect(near(style::vertical_align_shift("sub", 10.0f), -2.0f), "sub lowers 0.2em") &&
         expect(near(style::vertical_align_shift("-3px", 10.0f), -3.0f), "signed length") &&
         expect(near(style::vertical_align_shift("0.5em", 10.0f), 5.0f), "em length") &&
         expect(!style::is_supported_vertical_align("middle"), "middle rejected") &&
         expect(!style::is_supported_vertical_align("50%"), "percent rejected") &&
         expect(style::is_supported_vertical_align("SUPER"), "case-insensitive") && ok;
    return ok;
}

// --- 2. registry, shorthand, @supports ------------------------------------------------------

std::map<std::string, std::string> expanded(const PropertyRegistry& registry, const char* property,
                                            const char* value, std::size_t* diagnostics = nullptr) {
    const auto result = registry.expand(Declaration{property, value, false, 0});
    if (diagnostics != nullptr) {
        *diagnostics = result.diagnostics.size();
    }
    std::map<std::string, std::string> out;
    for (const Declaration& declaration : result.declarations) {
        out[declaration.property] = declaration.value;
    }
    return out;
}

bool registry_rules() {
    const PropertyRegistry registry;
    bool ok = expect(registry.find("text-decoration-line")->inherited, "line inherits") &&
              expect(!registry.find("vertical-align")->inherited, "vertical-align does not") &&
              expect(registry.find("vertical-align")->initial == "baseline", "initial baseline");

    const auto full = expanded(registry, "text-decoration", "underline line-through red 2px");
    ok = expect(full.size() == 4 && full.at("text-decoration-line") == "underline line-through" &&
                    full.at("text-decoration-color") == "red" &&
                    full.at("text-decoration-thickness") == "2px" &&
                    full.at("text-decoration-style") == "solid",
                "shorthand expands to the four longhands") &&
         ok;
    const auto reset = expanded(registry, "text-decoration", "underline");
    ok = expect(reset.at("text-decoration-color") == "currentColor" &&
                    reset.at("text-decoration-thickness") == "auto",
                "omitted parts reset to initial") &&
         ok;
    const auto none = expanded(registry, "text-decoration", "none");
    ok = expect(none.at("text-decoration-line") == "none", "shorthand none") && ok;
    const auto fn_color = expanded(registry, "text-decoration", "overline rgba(0, 0, 0, 0.5)");
    ok = expect(fn_color.at("text-decoration-color") == "rgba(0, 0, 0, 0.5)",
                "a functional colour stays one component") &&
         ok;

    std::size_t diagnostics = 0;
    ok =
        expect(expanded(registry, "text-decoration", "underline wavy", &diagnostics).empty() &&
                   diagnostics == 1,
               "wavy drops the whole shorthand with a diagnostic") &&
        expect(expanded(registry, "text-decoration", "underline underline", &diagnostics).empty() &&
                   diagnostics == 1,
               "a repeated line drops the shorthand") &&
        expect(expanded(registry, "text-decoration", "blink", &diagnostics).empty(),
               "an unknown component drops the shorthand") &&
        expect(expanded(registry, "vertical-align", "middle", &diagnostics).empty() &&
                   diagnostics == 1,
               "unsupported vertical-align is reported and dropped") &&
        expect(expanded(registry, "vertical-align", "super", &diagnostics).size() == 1 &&
                   diagnostics == 0,
               "super is accepted") &&
        expect(expanded(registry, "text-decoration-line", "line-through", &diagnostics).size() == 1,
               "longhand accepted") &&
        expect(expanded(registry, "text-decoration-color", "nope", &diagnostics).empty(),
               "bad colour dropped") &&
        ok;

    using style::evaluate_supports;
    using style::parse_supports_condition;
    const auto supports = [&](const char* text) {
        const auto parsed = parse_supports_condition(text);
        return parsed.ok && evaluate_supports(parsed.condition, registry);
    };
    ok = expect(supports("(text-decoration: underline red)"), "@supports shorthand") &&
         expect(!supports("(text-decoration: wavy)"), "@supports shorthand rejects wavy") &&
         expect(supports("(vertical-align: sub)"), "@supports sub") &&
         expect(!supports("(vertical-align: middle)"), "@supports middle is false") && ok;
    return ok;
}

// --- 3. computed-value mapping --------------------------------------------------------------

bool computed_mapping() {
    ComputedStyle computed;
    computed.set("font-size", "20px");
    computed.set("text-decoration-line", "underline overline");
    computed.set("text-decoration-thickness", "3px");
    computed.set("vertical-align", "super");
    const tessera::ui::text::TextStyle text = tessera::ui::text::text_style_from(computed);
    bool ok =
        expect(text.decoration_lines == (style::kDecorationUnderline | style::kDecorationOverline),
               "lines map") &&
        expect(near(text.decoration_thickness, 3.0f), "thickness maps") &&
        expect(near(text.baseline_shift, 6.6f), "super shift uses the computed size");
    const tessera::ui::text::TextStyle plain = tessera::ui::text::text_style_from(ComputedStyle{});
    ok = expect(plain.decoration_lines == 0 && plain.baseline_shift == 0.0f,
                "absent properties paint nothing extra") &&
         ok;
    return ok;
}

// --- 4. paint ------------------------------------------------------------------------------

struct Rec {
    Rect rect;
    Color color;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        rects.push_back({rect, color});
    }
    void fill_rounded_rect(const Rect&, const Color&, float) override {}
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        texts.push_back({run.rect, run.color});
        order.push_back('T');
    }
    std::vector<Rec> rects;
    std::vector<Rec> texts;
    std::string order;
};

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

ComputedStyle base_style(std::initializer_list<std::pair<const char*, const char*>> extra) {
    ComputedStyle computed;
    computed.set("font-size", "16px");
    computed.set("color", "rgb(10, 20, 30)");
    for (const auto& [property, value] : extra) {
        computed.set(property, value);
    }
    return computed;
}

RecordingSink paint_plain(std::initializer_list<std::pair<const char*, const char*>> extra,
                          PaintWrap wrap = PaintWrap::NoWrap, float width = 200.0f) {
    Node node(1);
    node.set_bounds({0.0f, 0.0f, width, 80.0f});
    PaintData data;
    data.text = "hello world";
    data.wrap = wrap;
    node.set_paint_data(data);
    StubStyles styles;
    styles.set(1, base_style(extra));
    RecordingSink sink;
    paint_scene(node, styles, sink);
    return sink;
}

bool paint_rules() {
    const RecordingSink none = paint_plain({});
    bool ok = expect(none.texts.size() == 1 && none.rects.empty(),
                     "no decoration emits no rect (unchanged output)");

    const RecordingSink under = paint_plain({{"text-decoration-line", "underline"}});
    ok = expect(under.texts.size() == 1 && under.rects.size() == 1, "one text, one rule") && ok;
    if (under.rects.size() == 1) {
        const Rect& rule = under.rects[0].rect;
        const Rect& run = under.texts[0].rect;
        ok = expect(near(rule.x, std::round(run.x)) && near(rule.height, 1.0f),
                    "underline starts at the run and is 1px at 16px") &&
             expect(rule.y > run.y + 8.0f && rule.y < run.y + run.height + 1.0f,
                    "underline sits below the baseline") &&
             expect(rule.width > 0.0f && near(rule.width, std::round(rule.width)),
                    "underline spans the line in whole pixels") &&
             expect(near(under.rects[0].color.r, under.texts[0].color.r) &&
                        near(under.rects[0].color.g, under.texts[0].color.g),
                    "currentColor follows the text colour") &&
             ok;
    }

    const RecordingSink all =
        paint_plain({{"text-decoration-line", "underline line-through overline"}});
    ok = expect(all.rects.size() == 3, "three lines, three rects") && ok;
    if (all.rects.size() == 3) {
        const float over = all.rects[0].rect.y;
        const float strike = all.rects[1].rect.y;
        const float under_y = all.rects[2].rect.y;
        ok = expect(over < strike && strike < under_y, "overline above strike above underline") &&
             ok;
    }

    const RecordingSink colored = paint_plain({{"text-decoration-line", "underline"},
                                               {"text-decoration-color", "rgb(255, 0, 0)"},
                                               {"text-decoration-thickness", "3px"}});
    ok =
        expect(colored.rects.size() == 1 && near(colored.rects[0].color.r, 1.0f) &&
                   near(colored.rects[0].color.g, 0.0f) && near(colored.rects[0].rect.height, 3.0f),
               "explicit colour and thickness") &&
        ok;

    const RecordingSink wrapped =
        paint_plain({{"text-decoration-line", "underline"}}, PaintWrap::Wrap, 40.0f);
    ok = expect(wrapped.texts.size() >= 2 && wrapped.rects.size() == wrapped.texts.size(),
                "a wrapped paragraph underlines every line") &&
         ok;
    return ok;
}

bool paint_shift() {
    const RecordingSink base = paint_plain({{"text-decoration-line", "underline"}});
    const RecordingSink up =
        paint_plain({{"vertical-align", "5px"}, {"text-decoration-line", "underline"}});
    const RecordingSink down = paint_plain({{"vertical-align", "sub"}});
    bool ok = expect(base.texts.size() == 1 && up.texts.size() == 1 && down.texts.size() == 1 &&
                         base.rects.size() == 1 && up.rects.size() == 1,
                     "shifted runs still paint once");
    if (!ok) {
        return false;
    }
    ok = expect(near(base.texts[0].rect.y - up.texts[0].rect.y, 5.0f), "text moves up 5px") &&
         expect(near(base.rects[0].rect.y - up.rects[0].rect.y, 5.0f), "its rule moves with it") &&
         expect(near(up.texts[0].rect.height, base.texts[0].rect.height),
                "the run box height (line box) is unchanged") &&
         expect(down.texts[0].rect.y > base.texts[0].rect.y, "sub moves the text down") && ok;
    return ok;
}

bool paint_spans() {
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 300.0f, 60.0f});
    auto text = std::make_unique<Node>(2);
    text->set_bounds({0.0f, 0.0f, 300.0f, 30.0f});
    PaintData data;
    data.text = "ab cd ef";
    data.spans = {PaintSpan{3, 3, 5}, PaintSpan{4, 6, 8}};
    text->set_paint_data(data);
    auto first = std::make_unique<Node>(3);
    first->set_element_kind("Text");
    auto second = std::make_unique<Node>(4);
    second->set_element_kind("Text");
    text->add_child(std::move(first));
    text->add_child(std::move(second));
    root.add_child(std::move(text));

    StubStyles styles;
    styles.set(1, base_style({}));
    styles.set(2, base_style({}));
    styles.set(3, base_style({{"text-decoration-line", "line-through"},
                              {"text-decoration-color", "rgb(0, 255, 0)"}}));
    styles.set(4, base_style({{"vertical-align", "super"}}));
    RecordingSink sink;
    paint_scene(root, styles, sink);

    bool ok = expect(sink.rects.size() == 1, "only the decorated span draws a rule");
    if (!ok) {
        return false;
    }
    ok = expect(near(sink.rects[0].color.g, 1.0f), "the span's own decoration colour") && ok;
    // Runs: "ab " (unattributed), "cd" (struck), " " , "ef" (raised). The raised run sits above
    // the unattributed one.
    float plain_y = -1.0f;
    float raised_y = -1.0f;
    for (std::size_t i = 0; i < sink.texts.size(); ++i) {
        if (i == 0) {
            plain_y = sink.texts[i].rect.y;
        }
        raised_y = sink.texts[i].rect.y;
    }
    ok = expect(raised_y < plain_y, "a super span paints above the line's other runs") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = grammar() && ok;
    ok = registry_rules() && ok;
    ok = computed_mapping() && ok;
    ok = paint_rules() && ok;
    ok = paint_shift() && ok;
    ok = paint_spans() && ok;
    if (!ok) {
        std::cerr << "text_decoration: failed\n";
        return 1;
    }
    std::cout << "text_decoration: ok\n";
    return 0;
}
