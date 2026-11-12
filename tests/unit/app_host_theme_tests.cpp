// SPDX-License-Identifier: MIT
//
// AppHost embedded theme registration (issue #81, ADR-048;
// docs/specs/stylesheet-v1.md sections 9.1-9.2). The host loads the generated blob (never the
// JSON), injects only the active theme's tokens as a generated `:root` rule through the ordinary
// cascade, and a theme switch replaces that rule, rebuilds the selector tree and drops the paint
// cache. A corrupt blob is reported and leaves the current theme applied. Painting goes through a
// deterministic CPU sink, so the assertions are exact RGBA bytes with no GPU, font or wall clock.

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::paint::PaintSink;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::parse_theme_asset;
using tessera::ui::style::serialize_theme_asset;
using tessera::ui::style::ThemeAsset;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "app_host_theme: " << message << "\n";
        ++failures;
    }
}

// The same deterministic CPU sink as app_host_stylesheet_tests: composites solid fill commands into
// an RGBA8 buffer, so a painted pixel is an exact byte assertion.
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

constexpr std::string_view kThemeJson = R"({
  "schemaVersion": 1,
  "start": "light",
  "themes": {
    "light": { "--color-bg": "#ffffff" },
    "dark":  { "--color-bg": "#101014" }
  }
})";

std::vector<std::uint8_t> theme_blob() {
    const auto parsed = parse_theme_asset(kThemeJson);
    if (!parsed) {
        std::cerr << "app_host_theme: the fixture theme does not parse\n";
        ++failures;
        return {};
    }
    return serialize_theme_asset(*parsed);
}

std::vector<std::uint8_t> blob_of(std::string_view json) {
    const auto parsed = parse_theme_asset(json);
    if (!parsed) {
        std::cerr << "app_host_theme: a fixture theme does not parse\n";
        ++failures;
        return {};
    }
    return serialize_theme_asset(*parsed);
}

// The property under test reads the token, so the injected value reaches the painted pixel.
constexpr std::string_view kBaseCss =
    ".probe { width: 100px; height: 50px; background-color: var(--color-bg); }";

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

void registration_and_switch() {
    Fixture fixture;
    tessera::app::AppHost host(fixture.root, compile_stylesheet(kBaseCss), fixture.loop,
                               fixture.config);

    // Before an asset is registered the built-in default theme is active and defines the reserved
    // chrome pair only.
    check(host.active_theme() == "default", "the built-in default is active before registration");
    check(host.theme_ids() == std::vector<std::string>{"default"},
          "the built-in asset has one theme id");
    const std::optional<std::string_view> builtin_background =
        host.theme_token("--window-background");
    check(builtin_background.has_value() && *builtin_background == "#ffffff",
          "the built-in reserved chrome pair is available");
    check(!host.theme_token("--color-bg").has_value(), "no app token before registration");

    const std::vector<std::uint8_t> blob = theme_blob();
    host.register_embedded_theme(blob);
    check(host.rejected_theme_count() == 0, "a loadable theme blob registers");
    check(host.active_theme() == "light", "registration applies the asset's start theme");
    check(host.theme_ids() == (std::vector<std::string>{"default", "light", "dark"}),
          "the theme ids are default plus the asset keys, in order");
    const std::optional<std::string_view> light_bg = host.theme_token("--color-bg");
    check(light_bg.has_value() && *light_bg == "#ffffff", "the light token is active");

    const tessera::app::HostFrame light_frame = host.begin_frame(0.0);
    CpuSink light_sink(320, 200);
    const tessera::ui::frame::FramePaint light_paint = host.paint(light_sink);
    check(light_frame.style_rebuilt, "registration dirties the selector tree");
    check(light_frame.frame_due, "registration makes a frame due");
    check(light_paint.painted, "registration repaints");
    check(fixture.probe->world_bounds().width == 100.0f, "the probe keeps its layout size");
    check(is_rgb(light_sink.pixel(10, 10), 255, 255, 255),
          "var(--color-bg) paints the light token");
    host.end_frame();

    // A theme switch replaces the injected rule and invalidates the paint cache.
    const std::uint64_t invalidations_before = host.paint_cache_stats().invalidations;
    check(host.set_theme("dark"), "an asset id switches");
    check(host.active_theme() == "dark", "the active theme is dark");
    check(host.paint_cache_stats().invalidations > invalidations_before,
          "the switch invalidates the paint cache");
    const std::optional<std::string_view> dark_bg = host.theme_token("--color-bg");
    check(dark_bg.has_value() && *dark_bg == "#101014", "the dark token is active");
    check(!host.set_theme("midnight"), "an unknown id is rejected");
    check(host.active_theme() == "dark", "a rejected switch keeps the active theme");

    const tessera::app::HostFrame dark_frame = host.begin_frame(1.0 / 60.0);
    CpuSink dark_sink(320, 200);
    (void)host.paint(dark_sink);
    check(dark_frame.style_rebuilt, "a theme switch rebuilds the selector tree");
    check(dark_frame.frame_due, "a theme switch makes a frame due");
    check(is_rgb(dark_sink.pixel(10, 10), 0x10, 0x10, 0x14), "the switch paints the dark token");
    host.end_frame();

    // A second registration replaces the asset rather than stacking.
    host.register_embedded_theme(blob_of(R"({"schemaVersion": 1, "start": "dark", "themes": {
        "dark": { "--color-bg": "#ff0000" }}})"));
    check(host.active_theme() == "dark", "the new asset's start theme applies");
    check(host.theme_ids() == (std::vector<std::string>{"default", "dark"}),
          "the new asset replaces the theme ids");
    (void)host.begin_frame(1.0 / 60.0);
    CpuSink replaced_sink(320, 200);
    (void)host.paint(replaced_sink);
    check(is_rgb(replaced_sink.pixel(10, 10), 255, 0, 0), "the replacement asset's token paints");
    host.end_frame();
}

void corrupt_blob_leaves_the_theme() {
    Fixture fixture;
    tessera::app::AppHost host(fixture.root, compile_stylesheet(kBaseCss), fixture.loop,
                               fixture.config);
    host.register_embedded_theme(theme_blob());
    host.set_theme("dark");
    (void)host.begin_frame(0.0);
    CpuSink before_sink(320, 200);
    (void)host.paint(before_sink);
    check(is_rgb(before_sink.pixel(10, 10), 0x10, 0x10, 0x14), "the dark theme paints first");
    host.end_frame();

    // A magic/format mismatch is the shape a stale embedded blob takes at run time.
    const std::uint8_t bad[] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x02, 0x03};
    host.register_embedded_theme(std::span<const std::uint8_t>(bad));
    check(host.rejected_theme_count() == 1, "the corrupt theme blob is counted as rejected");
    check(host.active_theme() == "dark", "a corrupt blob leaves the active theme untouched");
    check(host.theme_ids() == (std::vector<std::string>{"default", "light", "dark"}),
          "a corrupt blob leaves the asset untouched");

    const tessera::app::HostFrame frame = host.begin_frame(1.0 / 60.0);
    CpuSink after_sink(320, 200);
    // A rejected blob changes nothing, so the steady-state frame stays idle; forcing a full paint
    // shows the still-installed dark theme.
    (void)host.paint(after_sink, true);
    check(!frame.style_rebuilt, "a rejected blob does not dirty the selector tree");
    check(is_rgb(after_sink.pixel(10, 10), 0x10, 0x10, 0x14),
          "the previous theme still paints after the rejection");
    host.end_frame();

    // A later loadable registration clears the rejected count and applies its start theme.
    host.register_embedded_theme(theme_blob());
    check(host.rejected_theme_count() == 0, "a successful registration clears the rejected count");
    check(host.active_theme() == "light", "the new asset's start theme applies");
}

void reserved_pair_falls_back() {
    Fixture fixture;
    tessera::app::AppHost host(fixture.root, compile_stylesheet(kBaseCss), fixture.loop,
                               fixture.config);
    host.register_embedded_theme(blob_of(R"({"schemaVersion": 1, "themes": {
        "plain": { "--color-bg": "#00ff00", "--window-background": "#000000" }}})"));
    check(host.active_theme() == "default", "an absent start selects default");
    check(host.set_theme("plain"), "the plain theme activates");
    const std::optional<std::string_view> background = host.theme_token("--window-background");
    const std::optional<std::string_view> foreground = host.theme_token("--window-foreground");
    check(background.has_value() && *background == "#000000", "the theme-defined pair token wins");
    check(foreground.has_value() && *foreground == "#111111",
          "the missing reserved token falls back to the built-in value");
    const std::optional<std::string_view> missing = host.theme_token("--color-fg");
    check(!missing.has_value(), "an undefined non-reserved token has no value");
}

} // namespace

int main() {
    registration_and_switch();
    corrupt_blob_leaves_the_theme();
    reserved_pair_falls_back();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "app_host_theme: ok\n";
    return 0;
}
