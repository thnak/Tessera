// SPDX-License-Identifier: MIT

#include "tessera/ui/style/media_query.hpp"

#include <iostream>
#include <string_view>

namespace {

using tessera::ui::style::ColorScheme;
using tessera::ui::style::evaluate;
using tessera::ui::style::MediaContext;
using tessera::ui::style::parse_media_query_list;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "media_query: " << message << "\n";
    }
    return condition;
}

bool matches(std::string_view text, const MediaContext& context) {
    const auto parsed = parse_media_query_list(text);
    return parsed.ok && evaluate(parsed.list, context);
}

} // namespace

int main() {
    MediaContext context;
    if (!expect(matches("(min-width: 900px)", context), "min-width true") ||
        !expect(!matches("(min-width: 1400px)", context), "min-width false") ||
        !expect(matches("(max-width: 1400px)", context), "max-width true") ||
        !expect(matches("(min-height: 700px)", context), "min-height") ||
        !expect(matches("(orientation: landscape)", context), "landscape") ||
        !expect(!matches("(orientation: portrait)", context), "landscape not portrait") ||
        !expect(matches("screen and (orientation: landscape)", context), "screen and") ||
        !expect(!matches("print and (min-width: 1px)", context), "print false") ||
        !expect(matches("all", context), "all true")) {
        return 1;
    }

    if (!expect(matches("(prefers-color-scheme: light)", context), "color light") ||
        !expect(!matches("(prefers-color-scheme: dark)", context), "color dark false") ||
        !expect(matches("(prefers-reduced-motion: no-preference)", context), "motion default") ||
        !expect(!matches("(prefers-reduced-motion: reduce)", context), "motion reduce false")) {
        return 2;
    }

    context.color_scheme = ColorScheme::Dark;
    context.prefers_reduced_motion = true;
    if (!expect(matches("(prefers-color-scheme: dark)", context), "dark context") ||
        !expect(matches("(prefers-reduced-motion: reduce)", context), "reduce context")) {
        return 3;
    }

    context.resolution_dpi = 192.0;
    if (!expect(matches("(min-resolution: 2dppx)", context), "resolution dppx") ||
        !expect(matches("(resolution: 192dpi)", context), "resolution dpi") ||
        !expect(!matches("(min-resolution: 3dppx)", context), "resolution false")) {
        return 4;
    }

    if (!expect(matches("(max-width: 600px), (min-width: 1000px)", context), "comma or") ||
        !expect(matches("not (min-width: 2000px)", context), "not query") ||
        !expect(!matches("not (min-width: 900px)", context), "not query false")) {
        return 5;
    }

    context.viewport_width = 600.0;
    context.viewport_height = 900.0;
    if (!expect(matches("(max-width: 600px)", context), "exact width") ||
        !expect(matches("(orientation: portrait)", context), "portrait") ||
        !expect(matches("(max-width: 40rem)", context), "rem width")) {
        return 6;
    }

    if (!expect(!parse_media_query_list("").ok, "empty invalid") ||
        !expect(!parse_media_query_list("(min-width: 900px").ok, "unterminated invalid") ||
        !expect(!matches("(min-width: bad)", context), "invalid length false") ||
        !expect(!matches("(unknown-feature: 1px)", context), "unknown feature false")) {
        return 7;
    }

    // `@media (window-backdrop: ...)` matches the platform's resolved material, never a request
    // (glass-v1.md section 6). The default context is the null/headless answer: no material, so a
    // stylesheet's opaque fallback applies.
    {
        MediaContext backdrop_context;
        using tessera::hal::BackdropMaterial;
        if (!expect(matches("(window-backdrop: none)", backdrop_context),
                    "backdrop none default") ||
            !expect(!matches("(window-backdrop: mica)", backdrop_context),
                    "backdrop mica false by default")) {
            return 8;
        }
        backdrop_context.window_backdrop = BackdropMaterial::Mica;
        if (!expect(matches("(window-backdrop: mica)", backdrop_context), "backdrop mica") ||
            !expect(!matches("(window-backdrop: none)", backdrop_context),
                    "backdrop none false on mica") ||
            !expect(matches("NOT (window-backdrop: none)", backdrop_context), "backdrop negated")) {
            return 9;
        }
        backdrop_context.window_backdrop = BackdropMaterial::Acrylic;
        if (!expect(matches("(window-backdrop: acrylic)", backdrop_context), "backdrop acrylic") ||
            !expect(!matches("(window-backdrop: blur)", backdrop_context), "acrylic is not blur")) {
            return 10;
        }
        backdrop_context.window_backdrop = BackdropMaterial::Blur;
        if (!expect(matches("(window-backdrop: BLUR)", backdrop_context),
                    "backdrop keyword is case-insensitive") ||
            // `auto` is a request, never a resolved material: it cannot match.
            !expect(!matches("(window-backdrop: auto)", backdrop_context),
                    "backdrop auto never matches") ||
            !expect(!matches("(window-backdrop: fancy)", backdrop_context),
                    "unknown backdrop value does not match")) {
            return 11;
        }
    }

    std::cout << "media_query: ok\n";
    return 0;
}
