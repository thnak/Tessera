// SPDX-License-Identifier: MIT
//
// AppHost embedded-stylesheet registration (issue #81, ADR-048;
// docs/specs/compiled-stylesheet-v1.md section 10). The host loads the generated blob table,
// combines it with its constructor sheet in table (import-graph) order and replaces the cascade, so
// a later sheet wins an otherwise equal key. A corrupt blob is reported and skipped instead of
// crashing or leaving the previous registration in place. Painting goes through a deterministic CPU
// sink, so the assertions are exact RGBA bytes with no GPU, lavapipe, font or wall clock.

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::paint::PaintSink;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::CompiledStyleSheet;
using tessera::ui::style::EmbeddedStylesheet;
using tessera::ui::style::prepare_stylesheet;
using tessera::ui::style::PreparedStyleSheet;
using tessera::ui::style::serialize_stylesheet;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_host_stylesheet: " << message << "\n";
        ++failures;
    }
}

// A deterministic CPU paint sink: it composites the pass's solid commands into an RGBA8 buffer, so
// a test can assert the painted pixel color with no GPU or golden image. The pixel-center rule
// matches `CpuCanvas::fill_rect`; only the commands this fixture emits are implemented.
class CpuSink final : public PaintSink {
public:
    struct Pixel {
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
        std::uint8_t a = 255;
    };

    CpuSink(std::uint32_t width, std::uint32_t height)
        : width_(width),
          height_(height),
          pixels_(static_cast<std::size_t>(width) * height) {}

    void fill_rect(const Rect& rect, const Color& color) override {
        fill(rect, color);
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float /*radius*/) override {
        fill(rect, color);
    }

    [[nodiscard]] Pixel pixel(std::uint32_t x, std::uint32_t y) const {
        return pixels_[static_cast<std::size_t>(y) * width_ + x];
    }

private:
    static std::uint8_t channel(float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    void fill(const Rect& rect, const Color& color) {
        const Pixel value{channel(color.r), channel(color.g), channel(color.b), channel(color.a)};
        const float right = rect.x + rect.width;
        const float bottom = rect.y + rect.height;
        for (std::uint32_t y = 0; y < height_; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            if (py < rect.y || py >= bottom) {
                continue;
            }
            for (std::uint32_t x = 0; x < width_; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                if (px < rect.x || px >= right) {
                    continue;
                }
                pixels_[static_cast<std::size_t>(y) * width_ + x] = value;
            }
        }
    }

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<Pixel> pixels_;
};

bool is_rgb(const CpuSink::Pixel& value, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return value.r == r && value.g == g && value.b == b && value.a == 255;
}

std::vector<std::uint8_t> blob_of(std::string_view css) {
    return serialize_stylesheet(prepare_stylesheet(css));
}

struct Fixture {
    Node root{1};
    Node* probe = nullptr;
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop{clock};
    tessera::app::HostConfig config;

    Fixture() {
        auto probe_ptr = std::make_unique<Node>(2);
        probe = &root.add_child(std::move(probe_ptr));
        probe->set_element_kind("div");
        probe->set_class_name("probe");
        config.width = 320.0f;
        config.height = 200.0f;
    }
};

// The constructor sheet is the base app sheet: embedded sheets are appended after it and win an
// otherwise equal key, but its green shows through once nothing overrides it.
constexpr std::string_view kBaseCss =
    ".probe { width: 100px; height: 50px; background-color: #00ff00; }";

void registration_follows_table_order() {
    Fixture fixture;
    tessera::app::AppHost host(fixture.root, compile_stylesheet(kBaseCss), fixture.loop,
                               fixture.config);
    const std::vector<std::uint8_t> red =
        blob_of(".probe { width: 100px; height: 50px; background-color: #ff0000; }");
    const std::vector<std::uint8_t> blue =
        blob_of(".probe { width: 100px; height: 50px; background-color: #0000ff; }");

    // Table order [red, blue]: the later sheet wins the equal-specificity background.
    const EmbeddedStylesheet forward[] = {{"a.css", red}, {"b.css", blue}};
    host.register_embedded_stylesheets(forward);
    check(host.rejected_stylesheet_count() == 0, "a loadable table registers every sheet");

    const tessera::app::HostFrame registered = host.begin_frame(0.0);
    CpuSink sink(320, 200);
    const tessera::ui::frame::FramePaint painted = host.paint(sink);
    check(registered.style_rebuilt, "registration dirties the selector tree");
    check(registered.frame_due, "registration makes a frame due");
    check(painted.painted, "registration repaints");
    check(fixture.probe->world_bounds().width == 100.0f, "the embedded sheet sized the probe");
    check(is_rgb(sink.pixel(10, 10), 0, 0, 255), "the later embedded sheet wins the background");
    host.end_frame();

    // Reversing the table reverses the winner.
    const EmbeddedStylesheet reversed[] = {{"b.css", blue}, {"a.css", red}};
    host.register_embedded_stylesheets(reversed);
    const tessera::app::HostFrame revoked = host.begin_frame(1.0 / 60.0);
    CpuSink reversed_sink(320, 200);
    (void)host.paint(reversed_sink);
    check(revoked.style_rebuilt, "a second registration rebuilds the tree again");
    check(is_rgb(reversed_sink.pixel(10, 10), 255, 0, 0),
          "reversing the table reverses the winning sheet");
    host.end_frame();

    // An empty table returns the host to its constructor sheet with nothing rejected.
    host.register_embedded_stylesheets({});
    check(host.rejected_stylesheet_count() == 0, "an empty registration rejects nothing");
    const tessera::app::HostFrame cleared = host.begin_frame(1.0 / 60.0);
    CpuSink cleared_sink(320, 200);
    (void)host.paint(cleared_sink);
    check(cleared.style_rebuilt, "clearing the registration rebuilds the tree");
    check(is_rgb(cleared_sink.pixel(10, 10), 0, 255, 0),
          "an empty registration returns to the constructor sheet");
}

void corrupt_blob_is_reported_and_skipped() {
    Fixture fixture;
    tessera::app::AppHost host(fixture.root, compile_stylesheet(kBaseCss), fixture.loop,
                               fixture.config);
    const std::vector<std::uint8_t> red =
        blob_of(".probe { width: 100px; height: 50px; background-color: #ff0000; }");
    const std::vector<std::uint8_t> blue =
        blob_of(".probe { width: 100px; height: 50px; background-color: #0000ff; }");
    // A magic/format mismatch, the shape a stale embedded blob takes at run time.
    const std::array<std::uint8_t, 8> bytes{0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x02, 0x03};
    const std::span<const std::uint8_t> corrupt(bytes);

    // The corrupt entry is skipped (a TESSERA_CSS_BLOB_INVALID line goes to stderr); the sheets
    // around it still register in table order.
    const EmbeddedStylesheet mixed[] = {{"a.css", red}, {"broken.css", corrupt}, {"b.css", blue}};
    host.register_embedded_stylesheets(mixed);
    check(host.rejected_stylesheet_count() == 1, "the corrupt blob is counted as rejected");
    (void)host.begin_frame(0.0);
    CpuSink mixed_sink(320, 200);
    (void)host.paint(mixed_sink);
    check(is_rgb(mixed_sink.pixel(10, 10), 0, 0, 255),
          "the sheets after a corrupt one still register");
    host.end_frame();

    // Only a corrupt blob: the cascade is rebuilt from the constructor sheet alone. A skipped blob
    // is never applied and the previous registration is not left stale.
    const EmbeddedStylesheet only_corrupt[] = {{"broken.css", corrupt}};
    host.register_embedded_stylesheets(only_corrupt);
    check(host.rejected_stylesheet_count() == 1, "the only sheet is rejected");
    const tessera::app::HostFrame fallback = host.begin_frame(1.0 / 60.0);
    CpuSink fallback_sink(320, 200);
    (void)host.paint(fallback_sink);
    check(fallback.style_rebuilt, "the failed registration still rebuilds the tree");
    check(is_rgb(fallback_sink.pixel(10, 10), 0, 255, 0),
          "a rejected blob leaves the constructor sheet, not a stale registration");
}

// Library CSS reaches the host through the constructor sheet, already layered in `@layer
// tessera-ui` (ADR-048 decision 6). Its higher-specificity selector still loses to unlayered app
// CSS from the embedded table.
void library_constructor_sheet_loses_to_app_sheet() {
    Fixture fixture;
    const PreparedStyleSheet defaults =
        prepare_stylesheet("div.probe { width: 100px; height: 50px; background-color: #00ff00; }");
    const tessera::ui::style::StylesheetSource library_sources[] = {
        {"tessera-ui.css", &defaults, tessera::ui::style::Origin::Author, true}};
    const CompiledStyleSheet base(library_sources);
    tessera::app::AppHost host(fixture.root, base, fixture.loop, fixture.config);

    // Only the library sheet: its green applies (the layer alone changes nothing for one sheet).
    (void)host.begin_frame(0.0);
    CpuSink library_sink(320, 200);
    (void)host.paint(library_sink);
    check(is_rgb(library_sink.pixel(10, 10), 0, 255, 0),
          "the library constructor sheet paints its defaults");
    host.end_frame();

    // The app's unlayered `.probe` (lower specificity) still wins over `div.probe`.
    const std::vector<std::uint8_t> red =
        blob_of(".probe { width: 100px; height: 50px; background-color: #ff0000; }");
    const EmbeddedStylesheet app[] = {{"app.css", red}};
    host.register_embedded_stylesheets(app);
    (void)host.begin_frame(1.0 / 60.0);
    CpuSink app_sink(320, 200);
    (void)host.paint(app_sink);
    check(is_rgb(app_sink.pixel(10, 10), 255, 0, 0),
          "unlayered app CSS beats a higher-specificity library default");
}

} // namespace

int main() {
    registration_follows_table_order();
    corrupt_blob_is_reported_and_skipped();
    library_constructor_sheet_loses_to_app_sheet();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "app_host_stylesheet: ok\n";
    return 0;
}
