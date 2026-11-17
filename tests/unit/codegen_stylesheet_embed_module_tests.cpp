// SPDX-License-Identifier: MIT

// CSS module blobs (issue #463): the CLI compiles a `*.module.css` file into the embedded blob as
// its *scoped* rewrite, so the blob's selectors are the names the lowering puts on `className`
// values. A plain `.css` sheet keeps its authored names, so the change cannot scope a global sheet.
//
// The generated header writes the blob as hex byte text, so an authored or scoped class name is
// never a substring of the generated C++: the assertions decode the byte array (the bytes the app
// loads) and read the compiled selectors back. That is the level the issue asks for, reached
// through the real CLI driver rather than the stylesheet engine directly.
//
// Lives beside `codegen_stylesheet_embed_tests.cpp` because that file is at the 500-line hard
// limit; the CTest name keeps its prefix, so `-R codegen_stylesheet_embed` runs both.
//
// Deterministic: a temp project under the system temp directory and a fixed project-relative path,
// so the CSS module hash is the pinned `0b82a5` (css_module_scan_tests pins the same vector).

#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/selector.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32) && !defined(TESSERA_EMBED_TESTS_NO_SUBPROCESS)
#include <sys/wait.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(TESSERA_NO_SUBPROCESS) ||                                                              \
    (defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#define TESSERA_EMBED_TESTS_NO_SUBPROCESS 1
#endif

namespace {

namespace style = tessera::ui::style;
namespace fs = std::filesystem;

int failures = 0;

// Everything below the guard is used only by the CLI round-trip, which is compiled out where the
// test cannot spawn a subprocess (iOS/forbidden environments); defining it there would be an
// unused-function warning, which is an error under -Werror.
#if !defined(TESSERA_EMBED_TESTS_NO_SUBPROCESS)

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// The authored sheets of the fixture. `Card.module.css` is the module; `app.css` is a global sheet
// next to it, so the same run proves the scoped and the unscoped blob differ by the file's suffix.
constexpr std::string_view kModuleCss = ".card { color: red; }\n";
constexpr std::string_view kPlainCss = ".panel { color: blue; }\n";

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// The array initializer bytes of `kStylesheetBlob_<id>`, decoded back from the hex text (the
// generated header stores the blob as `0xNN` bytes, never as a source string).
std::vector<std::uint8_t> blob_bytes(std::string_view header, std::string_view symbol) {
    std::vector<std::uint8_t> bytes;
    const std::string marker = " " + std::string(symbol) + " = {";
    const std::size_t begin = header.find(marker);
    if (begin == std::string_view::npos) {
        return bytes;
    }
    const std::size_t end = header.find("};", begin);
    if (end == std::string_view::npos) {
        return bytes;
    }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    for (std::size_t i = begin; i + 3 < end; ++i) {
        if (header[i] != '0' || header[i + 1] != 'x') {
            continue;
        }
        const int high = digit(header[i + 2]);
        const int low = digit(header[i + 3]);
        if (high < 0 || low < 0) {
            continue;
        }
        bytes.push_back(static_cast<std::uint8_t>(high * 16 + low));
        i += 3;
    }
    return bytes;
}

// Every class name a decoded blob defines, in source order. A blob that would not load yields an
// empty list, so a malformed blob fails the assertions below instead of passing vacuously.
std::vector<std::string> blob_classes(std::span<const std::uint8_t> bytes) {
    std::vector<std::string> names;
    const auto loaded = style::load_stylesheet_blob(bytes);
    if (!loaded) {
        return names;
    }
    for (const style::PreparedItem& item : loaded->items) {
        if (item.kind != style::PreparedItemKind::Rule) {
            continue;
        }
        for (const style::ComplexSelector& complex : item.selectors.selectors) {
            for (const style::CompoundSelector& compound : complex.compounds) {
                names.insert(names.end(), compound.classes.begin(), compound.classes.end());
            }
        }
    }
    return names;
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

int run_cli(const fs::path& cli, const std::string& arguments, const fs::path& log) {
#ifdef _WIN32
    // cmd strips a leading quote from the whole /c command, so invoke through `call`.
    const std::string prefix = "call ";
#else
    const std::string prefix;
#endif
    const std::string command =
        prefix + quoted(cli) + " " + arguments + " > " + quoted(log) + " 2>&1";
    const int status = std::system(command.c_str());
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// A project with one screen importing a CSS module and a plain sheet, both for effect. The module
// is imported without binding its class map because that lowering form is a sibling slice; the
// file's suffix is what scopes it, so the blob is the same whichever import form the screen uses
// (docs/design/css-isolation.md).
void test_module_blob_is_scoped(const fs::path& cli) {
    const fs::path work = fs::temp_directory_path() / "tessera-codegen-css-module-embed";
    const fs::path project = work / "project";
    const fs::path out = work / "out";
    const fs::path log = work / "cli.log";
    std::error_code error;
    fs::remove_all(work, error);
    fs::create_directories(project / "ui");
    fs::create_directories(out);
    write_file(project / "tessera.json", R"({"schemaVersion":1,"app":{"namespace":"factory"}})");
    write_file(project / "ui" / "Card.module.css", kModuleCss);
    write_file(project / "ui" / "app.css", kPlainCss);
    write_file(project / "ui" / "App.tsx", "import \"./app.css\";\n"
                                           "import \"./Card.module.css\";\n"
                                           "function App() { return <View />; }\n");

    const std::string arguments =
        "--write --out " + quoted(out) + " --namespace demo " + quoted(project / "ui" / "App.tsx");
    check(run_cli(cli, arguments, log) == 0, "the module-sheet run exits 0");

    const fs::path header_path = out / "demo_stylesheets.hpp";
    check(fs::exists(header_path), "the run writes the stylesheet header");
    const std::string header = read_file(header_path);

    // The module blob: one rule, and its class is the scoped name the lowering will use on the
    // page. A `.card` here (the authored name) is the bug this slice closes.
    const std::vector<std::uint8_t> module_bytes =
        blob_bytes(header, "kStylesheetBlob_css_ui_Card_module");
    check(!module_bytes.empty(), "the header carries the module blob");
    const std::vector<std::string> module_classes = blob_classes(module_bytes);
    check(module_classes.size() == 1 && module_classes.front() == "card_0b82a5",
          "the module blob defines the scoped class name, not the authored one");
    check(module_bytes == style::serialize_stylesheet(
                              style::prepare_stylesheet(".card_0b82a5 { color: red; }\n")),
          "the module blob compiles the scoped rewrite of the file");

    // The plain sheet in the same run keeps its authored class name, byte for byte.
    const std::vector<std::uint8_t> plain_bytes = blob_bytes(header, "kStylesheetBlob_css_ui_app");
    check(!plain_bytes.empty(), "the header carries the plain blob");
    const std::vector<std::string> plain_classes = blob_classes(plain_bytes);
    check(plain_classes.size() == 1 && plain_classes.front() == "panel",
          "a plain sheet keeps its authored class name");
    check(plain_bytes == style::serialize_stylesheet(style::prepare_stylesheet(kPlainCss)),
          "the plain blob is the serializer output of the authored bytes: no renaming");

    fs::remove_all(work, error);
}

// A module file the scan rejects fails the build at the file's span, with the scan's own code: the
// blob cannot be compiled from a file whose scoping is unknown, and silently embedding the authored
// text would ship the unstyled page this slice exists to prevent.
void test_module_scan_error_fails_the_build(const fs::path& cli) {
    const fs::path work = fs::temp_directory_path() / "tessera-codegen-css-module-error";
    const fs::path project = work / "project";
    const fs::path out = work / "out";
    const fs::path log = work / "cli.log";
    std::error_code error;
    fs::remove_all(work, error);
    fs::create_directories(project / "ui");
    write_file(project / "tessera.json", R"({"schemaVersion":1,"app":{"namespace":"factory"}})");
    // The block form of `:global` is not supported in V1 (docs/design/css-isolation.md).
    write_file(project / "ui" / "Card.module.css", ":global { .card { color: red; } }\n");
    write_file(project / "ui" / "App.tsx",
               "import \"./Card.module.css\";\nfunction App() { return <View />; }\n");

    const std::string arguments =
        "--write --out " + quoted(out) + " --namespace demo " + quoted(project / "ui" / "App.tsx");
    check(run_cli(cli, arguments, log) == 1, "an unscannable module sheet exits 1");
    const std::string text = read_file(log);
    check(text.find("ui/Card.module.css:1:1: TESSERA_CSS_MODULE_GLOBAL_BLOCK") != std::string::npos,
          "the diagnostic is the scan's code at the file's span");
    check(!fs::exists(out / "demo_stylesheets.hpp"),
          "an unscannable module sheet produces no blob header");

    fs::remove_all(work, error);
}

#endif // TESSERA_EMBED_TESTS_NO_SUBPROCESS

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
#if !defined(TESSERA_EMBED_TESTS_NO_SUBPROCESS)
    if (argc != 2) {
        std::cerr << "usage: codegen_stylesheet_embed_module_tests CLI\n";
        return 2;
    }
    test_module_blob_is_scoped(fs::path(argv[1]).make_preferred());
    test_module_scan_error_fails_the_build(fs::path(argv[1]).make_preferred());
#else
    std::cout << "codegen stylesheet module embed tests skipped: subprocess execution is "
                 "unavailable on this platform\n";
#endif
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen stylesheet module embed tests passed\n";
    return 0;
}
