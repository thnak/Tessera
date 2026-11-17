// SPDX-License-Identifier: MIT

// tessera_scene_preview: the native half of the scene parity gates (issue #166, P2, and issue #168,
// P4, of docs/design/reference-equantic-ui.md section 7.4).
//
//   tessera_scene_preview --scene-json OUT FILE.tsx [--component NAME] [--salt SALT]
//   tessera_scene_preview --input-json OUT --script SCRIPT FILE.tsx [--component NAME] [--salt
//   SALT]
//
// Both modes run the same in-memory evaluation the browser module exports
// (`scene_evaluate.cpp` + `scene_session.cpp`), so each gate compares two artifacts instead of two
// implementations:
//   * `--scene-json` writes the canonical UiSnapshot document of docs/specs/automation-ids-v1.md
//     section 5.2, which scripts/ci/scene_snapshot_parity.py compares byte for byte;
//   * `--input-json --script` applies a script of clicks through the engine's own input router and
//     writes the state cells after every step, which scripts/ci/scene_input_parity.py compares.
//
// The viewport is the preview's fixed 320x240: the Wasm export hardcodes it, so the native side
// must use the same rectangle or the two would lay out different trees. `--salt` defaults to the
// same `preview` the module uses, for the same reason (the salt feeds every `uiId`).
//
// Exit codes: 0 document written; 1 the source produced no scene (the document is still written, so
// a caller can see what it got); 2 usage/setup.
//
// It is a separate executable rather than a verb on `tessera_codegen` on purpose: that CLI is the
// target the Wasm-compiler lane builds (docs/design/compiler-wasm.md section 2), and linking the
// engine (Yoga, the text stack, the stylesheet engine) into it would drag the whole UI into the P0
// Wasm artifact. The driver is shared instead of the binary.

#include "scene_interaction.hpp"
#include "scene_preview.hpp"
#include "tessera/hal/fold_posture.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kUsage =
    "usage: tessera_scene_preview --scene-json OUT|FILE.tsx [options] FILE.tsx\n"
    "       tessera_scene_preview --input-json OUT --script SCRIPT [options] FILE.tsx\n"
    "\n"
    "  --scene-json OUT   write the canonical scene snapshot document to OUT (`-` for stdout)\n"
    "  --input-json OUT   write the input-script result document to OUT (`-` for stdout)\n"
    "  --script FILE      the input script: one `click <automationId>` step per line (`-` for "
    "stdin)\n"
    "  --component NAME   the component to preview (default: the first in the file)\n"
    "  --file NAME        the project-relative source name the ids are hashed with (default: the\n"
    "                     input path with POSIX separators); the browser module passes the same\n"
    "                     name, and the source path is an input to every uiId, so the parity gate\n"
    "                     must give both sides one spelling\n"
    "  --salt SALT        the project salt the ids are hashed with (default: preview)\n"
    "  --size WxH         the layout viewport in logical pixels (default: 320x240)\n"
    "  --fold POSTURE     simulate a foldable posture: flat, book (half-opened, vertical hinge)\n"
    "                     or tabletop (half-opened, horizontal hinge). Default: no fold. This is\n"
    "                     the native half of the studio's foldable control (issue #408):\n"
    "                     `tessera_scene_json_folded` passes the same value to the same driver, "
    "so\n"
    "                     a posture's snapshot can be compared across the two.\n"
    "  --stylesheet FILE  the author stylesheet (issue #390), the page's second textarea (`-` for\n"
    "                     stdin). Filed as the virtual file ui/Preview.css, so a source's\n"
    "                     `import \"./Preview.css\";` resolves to it. When given, it REPLACES the\n"
    "                     preview's built-in sheet; without it, the built-in sheet is the "
    "fallback.\n"
    "                     This is the native half of author CSS: the same text the page sends to\n"
    "                     `tessera_scene_json_styled` produces the same document here.\n"
    "\n"
    "Exit codes: 0 document written; 1 no scene (an empty `nodes` array); 2 usage/setup.\n";

int usage_error(std::string_view message) {
    std::cerr << "tessera_scene_preview: " << message << "\n\n" << kUsage;
    return 2;
}

bool read_file(const std::string& path, std::string& out) {
    if (path == "-") {
        out.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// `WxH`, both positive finite. The preview's viewport is what the layout and the fitted window
// agree on, so a malformed size is a usage error rather than a silent default.
bool parse_size(std::string_view text, float& width, float& height) {
    const std::size_t x = text.find('x');
    if (x == std::string_view::npos) {
        return false;
    }
    try {
        const std::string left(text.substr(0, x));
        const std::string right(text.substr(x + 1));
        width = std::stof(left);
        height = std::stof(right);
    } catch (const std::exception&) {
        return false;
    }
    return width > 0.0f && height > 0.0f && width <= 65535.0f && height <= 65535.0f;
}

bool write_document(const std::string& output, const std::string& document) {
    if (output == "-") {
        std::cout << document;
        return true;
    }
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << document;
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char** argv) {
    std::string scene_output;
    std::string input_output;
    std::string script_path;
    std::string component;
    std::string project_file;
    std::string stylesheet_path;
    std::string salt = "preview";
    float width = 320.0f;
    float height = 240.0f;
    tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None;
    std::string input;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout << kUsage;
            return 0;
        }
        if (arg == "--scene-json" || arg == "--input-json" || arg == "--script" ||
            arg == "--component" || arg == "--salt" || arg == "--size" || arg == "--file" ||
            arg == "--fold" || arg == "--stylesheet") {
            if (i + 1 >= argc) {
                return usage_error("`" + std::string(arg) + "` needs a value");
            }
            const std::string value(argv[++i]);
            if (arg == "--scene-json") {
                scene_output = value;
            } else if (arg == "--input-json") {
                input_output = value;
            } else if (arg == "--script") {
                script_path = value;
            } else if (arg == "--component") {
                component = value;
            } else if (arg == "--file") {
                project_file = value;
            } else if (arg == "--stylesheet") {
                stylesheet_path = value;
            } else if (arg == "--salt") {
                salt = value;
            } else if (arg == "--fold") {
                const std::optional<tessera::hal::FoldPosture> parsed =
                    tessera::hal::parse_fold_posture(value);
                if (!parsed) {
                    return usage_error("`--fold` takes flat, book or tabletop, got `" + value +
                                       "`");
                }
                posture = *parsed;
            } else if (!parse_size(value, width, height)) {
                return usage_error("`--size` takes WxH, for example 320x240");
            }
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return usage_error("unknown option `" + std::string(arg) + "`");
        }
        if (!input.empty()) {
            return usage_error("one input .tsx file");
        }
        input = std::string(arg);
    }

    if (scene_output.empty() == input_output.empty()) {
        return usage_error("exactly one of --scene-json and --input-json is required");
    }
    if (input_output.empty() != script_path.empty()) {
        return usage_error("--input-json needs --script, and --script needs --input-json");
    }
    if (input.empty()) {
        return usage_error("no input .tsx file");
    }
    std::string source;
    if (!read_file(input, source)) {
        std::cerr << "tessera_scene_preview: cannot read `" << input << "`\n";
        return 2;
    }

    tessera::compiler::codegen::cli::ScenePreviewOptions options;
    options.file = project_file.empty() ? std::string_view(input) : std::string_view(project_file);
    options.salt = salt;
    options.width = width;
    options.height = height;
    options.posture = posture;
    // The author stylesheet (issue #390). Read like the source, so `--stylesheet -` takes it from
    // stdin and the parity script can drive both sides with one string. Its text has to outlive
    // `options`, which borrows it.
    std::string stylesheet;
    if (!stylesheet_path.empty() && !read_file(stylesheet_path, stylesheet)) {
        std::cerr << "tessera_scene_preview: cannot read `" << stylesheet_path << "`\n";
        return 2;
    }
    options.stylesheet = stylesheet;

    std::string document;
    if (!input_output.empty()) {
        std::string script;
        if (!read_file(script_path, script)) {
            std::cerr << "tessera_scene_preview: cannot read script `" << script_path << "`\n";
            return 2;
        }
        document = tessera::compiler::codegen::cli::scene_input_document(source, options, component,
                                                                         script);
    } else {
        document =
            tessera::compiler::codegen::cli::scene_preview_document(source, options, component);
    }

    const std::string& output = !input_output.empty() ? input_output : scene_output;
    // A document with no nodes is a successful write of a failed preview: exit 1 says "the source
    // produced no scene" while the bytes still land where the caller asked, the way
    // `--diagnostics-json` writes a document on a failed run.
    const bool produced_scene = document.find("\"nodes\":[]") == std::string::npos &&
                                document.find("\"error\":") == std::string::npos;
    if (!write_document(output, document)) {
        std::cerr << "tessera_scene_preview: cannot write `" << output << "`\n";
        return 2;
    }
    return produced_scene ? 0 : 1;
}
