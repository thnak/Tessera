// SPDX-License-Identifier: MIT
//
// `tessera theme --seed <color>` -- generate an Instrument token set from one accent color
// (issue #79, ADR-047 item 5).
//
// Writes a complete `assets.theme` file: `instrument-light`, `instrument-dark` and
// `instrument-high-contrast`, sharing one token name set, every text and indicator pair cleared
// against the WCAG 2.2 floors. `--check FILE` runs the same contrast checker over an existing token
// set, hand-edited or generated, and exits 1 with one line per failing pair.
//
// Exit codes: 0 success, 1 contrast or unsolvable-seed failure, 2 usage or IO.

#include "tessera/ui/style/contrast.hpp"
#include "tessera/ui/style/theme_asset.hpp"
#include "tessera/ui/style/theme_generate.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace style = tessera::ui::style;

void usage(const char* program) {
    std::printf(
        "usage: %s theme --seed <color> [--out FILE] [options]\n"
        "       %s theme --check FILE\n"
        "\n"
        "  --seed <color>     accent color: #rgb, #rrggbb, rgb(...) or a CSS color name\n"
        "  --out <file>       write the token set here (default: standard output)\n"
        "  --prefix <id>      theme id prefix (default: instrument), giving <prefix>-light,\n"
        "                     <prefix>-dark and <prefix>-high-contrast\n"
        "  --version <semver> theme version stamped on every theme (default: 1.0.0; empty skips)\n"
        "  --no-accent-scale  omit the --accent-50 ... --accent-950 tonal steps\n"
        "  --check <file>     lint an existing token set instead of generating one: every text\n"
        "                     and indicator pair against the WCAG 2.2 AA floors, exit 1 on a miss\n"
        "  --quiet            on success, print nothing\n"
        "\n"
        "The output is deterministic: the same seed and options always produce identical bytes.\n",
        program, program);
}

struct Arguments {
    std::string seed;
    std::string out;
    std::string prefix = "instrument";
    std::string version = "1.0.0";
    std::string check;
    bool include_accent_scale = true;
    bool quiet = false;
};

[[nodiscard]] bool next_argument(const char* program, int argc, char** argv, int& index,
                                 std::string& out) {
    if (index + 1 >= argc) {
        std::fprintf(stderr, "tessera theme: %s needs a value\n", argv[index]);
        usage(program);
        return false;
    }
    ++index;
    out = argv[index];
    return true;
}

[[nodiscard]] std::string read_file(const std::string& path, bool& ok) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ok = false;
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    ok = true;
    return text.str();
}

[[nodiscard]] bool write_file(const std::string& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return file.good();
}

// `--check`: parse the token set with the theme asset parser, then run the shared contrast
// checker. A parse error and a contrast finding both exit 1; the message names the theme and the
// pair so the author can find the token to edit.
int run_check(const Arguments& arguments) {
    bool ok = false;
    const std::string text = read_file(arguments.check, ok);
    if (!ok) {
        std::fprintf(stderr, "tessera theme: cannot read %s\n", arguments.check.c_str());
        return 2;
    }
    const auto parsed = style::parse_theme_asset(text);
    if (!parsed) {
        std::fprintf(stderr, "tessera theme: %s: %s\n", arguments.check.c_str(),
                     parsed.error().message.c_str());
        return 1;
    }
    for (const std::string& warning : parsed->warnings) {
        std::fprintf(stderr, "tessera theme: %s\n", warning.c_str());
    }
    // The strictest floors apply to the theme whose id says it is high contrast; a name a project
    // chooses differently still gets the normal floors, and the asset it names is checked either
    // way. This is the one piece of naming the checker cannot infer, so it is documented in
    // docs/specs/theme-generation-v1.md rather than guessed.
    std::string high_contrast;
    for (const style::Theme& theme : parsed->themes) {
        if (theme.id.ends_with("-high-contrast") || theme.id == "high-contrast") {
            high_contrast = theme.id;
        }
    }
    const std::vector<style::ContrastFinding> findings =
        style::check_theme_contrast(*parsed, high_contrast);
    for (const style::ContrastFinding& finding : findings) {
        std::fprintf(stderr, "%s\n", style::describe_contrast_finding(finding).c_str());
    }
    if (!findings.empty()) {
        return 1;
    }
    if (!arguments.quiet) {
        std::printf("tessera theme: %s: %zu theme(s) meet every WCAG 2.2 AA contrast floor\n",
                    arguments.check.c_str(), parsed->themes.size());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    // Accept the documented `tessera theme ...` form as well as a bare argument list, so the
    // future `tessera` driver can forward its arguments unchanged (the same shape as
    // `tessera icons`).
    int index = 1;
    if (index < argc && std::string_view(argv[index]) == "theme") {
        ++index;
    }
    for (; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--seed") {
            if (!next_argument(argv[0], argc, argv, index, arguments.seed)) {
                return 2;
            }
        } else if (argument == "--out") {
            if (!next_argument(argv[0], argc, argv, index, arguments.out)) {
                return 2;
            }
        } else if (argument == "--prefix") {
            if (!next_argument(argv[0], argc, argv, index, arguments.prefix)) {
                return 2;
            }
        } else if (argument == "--version") {
            if (!next_argument(argv[0], argc, argv, index, arguments.version)) {
                return 2;
            }
        } else if (argument == "--check") {
            if (!next_argument(argv[0], argc, argv, index, arguments.check)) {
                return 2;
            }
        } else if (argument == "--no-accent-scale") {
            arguments.include_accent_scale = false;
        } else if (argument == "--quiet") {
            arguments.quiet = true;
        } else if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            return 0;
        } else if (!argument.empty() && argument.front() == '-') {
            std::fprintf(stderr, "tessera theme: unknown option %s\n", argv[index]);
            usage(argv[0]);
            return 2;
        } else {
            std::fprintf(stderr, "tessera theme: unexpected argument %s\n", argv[index]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!arguments.check.empty()) {
        return run_check(arguments);
    }
    if (arguments.seed.empty()) {
        usage(argv[0]);
        return 2;
    }

    style::ThemeGenerateOptions options;
    options.id_prefix = arguments.prefix;
    options.version = arguments.version;
    options.include_accent_scale = arguments.include_accent_scale;

    const auto asset =
        style::generate_theme_asset(arguments.seed, options, style::instrument_variant_specs());
    if (!asset) {
        std::fprintf(stderr, "tessera theme: %s: %s\n",
                     style::theme_generate_error_code_name(asset.error().code),
                     asset.error().message.c_str());
        return 1;
    }
    const std::string text = style::format_theme_asset(*asset);
    if (arguments.out.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    if (!write_file(arguments.out, text)) {
        std::fprintf(stderr, "tessera theme: cannot write %s\n", arguments.out.c_str());
        return 2;
    }
    if (!arguments.quiet) {
        std::printf("tessera theme: wrote %zu theme(s) to %s\n", asset->themes.size(),
                    arguments.out.c_str());
    }
    return 0;
}
