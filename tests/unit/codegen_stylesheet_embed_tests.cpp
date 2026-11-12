// SPDX-License-Identifier: MIT

// Embedded stylesheet blobs (issue #81, ADR-048): the `<module>_stylesheets.hpp` file codegen emits
// for a module with blobs, its ordering/identifier/determinism rules, the warning and error matrix,
// and the CLI path that compiles CSS into the blobs and loads them back with load_stylesheet_blob.
// Deterministic: hand-built IR, literal byte blobs, and a temp project under the system temp dir.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;
namespace style = tessera::ui::style;
namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

int finish(std::string_view name) {
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << name << " passed\n";
    return 0;
}

// The CLI round-trip below is the only user of these helpers, and it is compiled out where the
// test cannot spawn a subprocess (iOS/forbidden environments); defining them there would be an
// unused-function warning, which is an error under -Werror.
#if !defined(TESSERA_EMBED_TESTS_NO_SUBPROCESS)
std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The sibling tests call this `write_file(std::string, ...)`; a path overload keeps both readable.
void write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}
#endif // TESSERA_EMBED_TESTS_NO_SUBPROCESS

std::string first_error(const codegen::CodegenResult& result) {
    if (result) {
        return {};
    }
    return result.error().empty() ? std::string("<empty>") : result.error().front();
}

ir::IrModule make_module() {
    ir::IrModule module;
    module.name = "demo";
    module.stylesheets = {ir::StylesheetRef{"css::ui/app", "ui/app.css", "fnv1a64:0"},
                          ir::StylesheetRef{"css::ui/sub/card", "ui/sub/card.css", "fnv1a64:1"}};
    return module;
}

std::vector<std::uint8_t> app_bytes() {
    return {0x00, 0x01, 0xfe, 0xff};
}

std::vector<std::uint8_t> card_bytes() {
    std::vector<std::uint8_t> bytes;
    for (std::uint8_t value = 0; value < 14; ++value) {
        bytes.push_back(value); // more than one 12-byte line, so the wrapping is pinned
    }
    return bytes;
}

std::vector<codegen::StylesheetEmbed> make_embeds() {
    return {codegen::StylesheetEmbed{"ui/app.css", app_bytes()},
            codegen::StylesheetEmbed{"ui/sub/card.css", card_bytes()}};
}

const codegen::GeneratedFile* find_file(const std::vector<codegen::GeneratedFile>& files,
                                        std::string_view path) {
    for (const codegen::GeneratedFile& file : files) {
        if (file.path == path) {
            return &file;
        }
    }
    return nullptr;
}

// The array initializer bytes of `kStylesheetBlob_<id>`, decoded back from the hex text.
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
    const std::string_view block = header.substr(begin, end - begin);
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i + 3 < block.size(); ++i) {
        if (block[i] != '0' || block[i + 1] != 'x') {
            continue;
        }
        const int high = digit(block[i + 2]);
        const int low = digit(block[i + 3]);
        if (high < 0 || low < 0) {
            continue;
        }
        bytes.push_back(static_cast<std::uint8_t>(high * 16 + low));
        i += 3;
    }
    return bytes;
}

void test_embedded_header_is_pinned() {
    const auto embeds = make_embeds();
    const auto result = codegen::generate(make_module(), embeds);
    check(result.has_value(), "a module with blobs generates: " + first_error(result));
    if (!result) {
        return;
    }
    check_eq(std::to_string(result->size()), "1", "one component-less module emits only the table");
    const codegen::GeneratedFile* header = find_file(*result, "demo_stylesheets.hpp");
    check(header != nullptr, "the extra file is <module>_stylesheets.hpp");
    if (header == nullptr) {
        return;
    }
    const std::string expected =
        "// Generated by tessera_codegen. Do not edit.\n"
        "// Embedded stylesheet blobs (docs/specs/compiled-stylesheet-v1.md): load with\n"
        "// tessera::ui::style::load_stylesheet_blob at startup.\n"
        "#pragma once\n"
        "#include <array>\n"
        "#include <cstdint>\n"
        "#include \"tessera/ui/style/stylesheet_blob.hpp\"\n"
        "namespace demo {\n"
        "  inline constexpr std::array<std::uint8_t, 4> kStylesheetBlob_css_ui_app = {\n"
        "    0x00, 0x01, 0xfe, 0xff,\n"
        "  };\n"
        "\n"
        "  inline constexpr std::array<std::uint8_t, 14> kStylesheetBlob_css_ui_sub_card = {\n"
        "    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,\n"
        "    0x0c, 0x0d,\n"
        "  };\n"
        "\n"
        "  inline constexpr std::array<tessera::ui::style::EmbeddedStylesheet, 2> "
        "kEmbeddedStylesheets = {{\n"
        "    { \"ui/app.css\", kStylesheetBlob_css_ui_app },\n"
        "    { \"ui/sub/card.css\", kStylesheetBlob_css_ui_sub_card },\n"
        "  }};\n"
        "} // namespace demo\n";
    check_eq(header->text, expected, "the emitted header is pinned byte-for-byte");
    check(codegen::codegen_warnings(make_module(), embeds).empty(),
          "an embedded sheet has no not-embedded warning");

    check(blob_bytes(header->text, "kStylesheetBlob_css_ui_app") == app_bytes(),
          "the first array decodes back to its blob");
    check(blob_bytes(header->text, "kStylesheetBlob_css_ui_sub_card") == card_bytes(),
          "the second array decodes back to its blob");
    check(header->text.find("std::array<std::uint8_t, 4> kStylesheetBlob_css_ui_app") !=
              std::string::npos,
          "the array size is the blob size");
    check(header->text.find("std::array<std::uint8_t, 14> kStylesheetBlob_css_ui_sub_card") !=
              std::string::npos,
          "the second array size is the second blob size");
}

void test_partial_embeds_keep_the_warning() {
    const std::vector<codegen::StylesheetEmbed> embeds{
        {"ui/sub/card.css", card_bytes()}}; // ui/app.css has no blob
    const auto result = codegen::generate(make_module(), embeds);
    check(result.has_value(), "a partial embed list generates");
    if (!result) {
        return;
    }
    const codegen::GeneratedFile* header = find_file(*result, "demo_stylesheets.hpp");
    check(header != nullptr, "the file is still emitted");
    if (header == nullptr) {
        return;
    }
    check(header->text.find("kStylesheetBlob_css_ui_app") == std::string::npos,
          "a sheet without a blob is absent from the arrays");
    check(header->text.find("kStylesheetBlob_css_ui_sub_card") != std::string::npos,
          "the embedded sheet is present");
    check(header->text.find("std::array<tessera::ui::style::EmbeddedStylesheet, 1> "
                            "kEmbeddedStylesheets") != std::string::npos,
          "the table holds only the embedded sheet");
    check(header->text.find("{ \"ui/sub/card.css\", kStylesheetBlob_css_ui_sub_card }") !=
              std::string::npos,
          "the table entry names the embedded path");

    const std::vector<std::string> warnings = codegen::codegen_warnings(make_module(), embeds);
    check_eq(std::to_string(warnings.size()), "1", "only the sheet without a blob warns");
    if (!warnings.empty()) {
        check(warnings[0].starts_with("TESSERA_CG_STYLESHEET_NOT_EMBEDDED:"), "the warning code");
        check(warnings[0].find("ui/app.css") != std::string::npos, "the warning names the sheet");
    }
}

void test_without_embeds_nothing_changes() {
    const auto result = codegen::generate(make_module());
    check(result.has_value() && result->empty(),
          "no blob means no extra file (and no component files here)");
    const std::vector<std::string> warnings = codegen::codegen_warnings(make_module());
    check_eq(std::to_string(warnings.size()), "2", "every sheet still warns");
    if (warnings.size() == 2) {
        check(warnings[0].find("ui/app.css") != std::string::npos, "module order, first");
        check(warnings[1].find("ui/sub/card.css") != std::string::npos, "module order, second");
    }
}

void test_unknown_embed_is_an_error() {
    const std::vector<codegen::StylesheetEmbed> unknown{{"ui/other.css", {1, 2, 3}}};
    const auto result = codegen::generate(make_module(), unknown);
    check(!result.has_value(), "a blob for an unknown path is rejected");
    check_eq(first_error(result),
             "TESSERA_CODEGEN_STYLESHEET_UNKNOWN: stylesheet blob for `ui/other.css` is not a "
             "stylesheet of this module",
             "the unknown-blob diagnostic");

    // Deterministic ordering: the first unknown path in sorted order is reported, whatever order
    // the caller supplied them in.
    const std::vector<codegen::StylesheetEmbed> two_unknown{{"ui/z.css", {1}}, {"ui/a.css", {2}}};
    const auto sorted = codegen::generate(make_module(), two_unknown);
    check(!sorted.has_value() && first_error(sorted).find("`ui/a.css`") != std::string::npos,
          "the first unknown path in sorted order is reported");
}

void test_determinism_and_module_order() {
    const auto embeds = make_embeds();
    const auto first = codegen::generate(make_module(), embeds);
    const auto second = codegen::generate(make_module(), embeds);
    check(first.has_value() && second.has_value(), "the module generates twice");
    if (!first || !second) {
        return;
    }
    check_eq(std::to_string(first->size()), std::to_string(second->size()), "same file count");
    for (std::size_t i = 0; i < first->size() && i < second->size(); ++i) {
        check_eq((*first)[i].path, (*second)[i].path, "same file path");
        check_eq((*first)[i].text, (*second)[i].text, "same file bytes");
    }

    // The table follows module.stylesheets order, not the caller's embed order.
    std::vector<codegen::StylesheetEmbed> reversed = embeds;
    std::reverse(reversed.begin(), reversed.end());
    const auto reordered = codegen::generate(make_module(), reversed);
    check(reordered.has_value(), "a reordered embed list generates");
    const codegen::GeneratedFile* original_header = find_file(*first, "demo_stylesheets.hpp");
    const codegen::GeneratedFile* reordered_header =
        reordered ? find_file(*reordered, "demo_stylesheets.hpp") : nullptr;
    check(original_header != nullptr && reordered_header != nullptr,
          "both runs emit the stylesheet header");
    if (original_header != nullptr && reordered_header != nullptr) {
        check_eq(reordered_header->text, original_header->text,
                 "the emitted bytes do not depend on the caller's embed order");
    }
}

#if !defined(TESSERA_EMBED_TESTS_NO_SUBPROCESS)

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

int run_cli(const fs::path& cli, const std::string& arguments, const fs::path& log_dir) {
    const fs::path log = log_dir / "cli.log";
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

constexpr std::string_view kCss = ".card { color: #ff0000; padding: 4px 8px; }\n"
                                  "@media (min-width: 600px) { .card { padding: 16px; } }\n";

bool same_items(const std::vector<style::PreparedItem>& left,
                const std::vector<style::PreparedItem>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].kind != right[i].kind || left[i].media.size() != right[i].media.size() ||
            left[i].declarations.size() != right[i].declarations.size() ||
            left[i].layer != right[i].layer) {
            return false;
        }
        for (std::size_t d = 0; d < left[i].declarations.size(); ++d) {
            if (left[i].declarations[d].property != right[i].declarations[d].property ||
                left[i].declarations[d].value != right[i].declarations[d].value ||
                left[i].declarations[d].important != right[i].declarations[d].important) {
                return false;
            }
        }
    }
    return true;
}

// A project with one TSX screen importing `./app.css`; a salt (tessera.json) makes the paths
// project-relative, so the emitted table names `ui/app.css` deterministically.
struct TempProject {
    fs::path work;
    fs::path project;
    fs::path out;
    fs::path log_dir;

    explicit TempProject(bool make_output) {
        work = fs::temp_directory_path() / "tessera-codegen-stylesheet-embed";
        project = work / "project";
        out = work / "out";
        log_dir = work / "logs";
        std::error_code error;
        fs::remove_all(work, error);
        fs::create_directories(project / "ui");
        fs::create_directories(log_dir);
        if (make_output) {
            fs::create_directories(out);
        }
        write_file(project / "tessera.json",
                   R"({"schemaVersion":1,"app":{"namespace":"factory"}})");
        write_file(project / "ui" / "App.tsx",
                   "import \"./app.css\";\nfunction App() { return <View />; }\n");
    }

    void write_css(std::string_view text) const {
        write_file(project / "ui" / "app.css", text);
    }

    [[nodiscard]] std::string arguments(bool check_only) const {
        return std::string(check_only ? "--check" : "--write") + " --out " + quoted(out) +
               " --namespace demo " + quoted(project / "ui" / "App.tsx");
    }
};

void test_cli_embeds_and_loads(const fs::path& cli) {
    TempProject project(true);
    project.write_css(kCss);
    check(run_cli(cli, project.arguments(false), project.log_dir) == 0,
          "the embedding run exits 0");
    const std::string log = read_file(project.log_dir / "cli.log");
    check(log.find("TESSERA_CG_STYLESHEET_NOT_EMBEDDED") == std::string::npos,
          "an embedded sheet has no not-embedded warning");

    const fs::path header_path = project.out / "demo_stylesheets.hpp";
    check(fs::exists(header_path), "the run writes <module>_stylesheets.hpp");
    const std::string header = read_file(header_path);
    check(header.find("\"ui/app.css\"") != std::string::npos, "the table names the project path");
    check(header.find("kStylesheetBlob_css_ui_app") != std::string::npos,
          "the stylesheet id is sanitized into the array name");

    const std::vector<std::uint8_t> bytes = blob_bytes(header, "kStylesheetBlob_css_ui_app");
    const style::PreparedStyleSheet direct = style::prepare_stylesheet(kCss);
    const std::vector<std::uint8_t> serialized = style::serialize_stylesheet(direct);
    check(bytes == serialized, "the emitted bytes are exactly the serializer output");

    const auto loaded = style::load_stylesheet_blob(bytes);
    check(loaded.has_value(), "the emitted bytes load with load_stylesheet_blob");
    if (loaded) {
        check(loaded->source_hash == direct.source_hash, "the loaded source hash matches");
        check(loaded->layers == direct.layers, "the loaded layer table matches");
        check(same_items(loaded->items, direct.items),
              "the loaded items match a direct prepare of the same CSS");
    }

    // A second regeneration is a byte- and mtime-no-op, and --check accepts it.
    const auto mtime = fs::last_write_time(header_path);
    check(run_cli(cli, project.arguments(false), project.log_dir) == 0, "a second run exits 0");
    check(read_file(header_path) == header, "a second run is byte-identical");
    check(fs::last_write_time(header_path) == mtime, "a second run does not rewrite the header");
    check(run_cli(cli, project.arguments(true), project.log_dir) == 0,
          "--check accepts the embedded output");

    // A CSS edit changes the blob and is drift for --check.
    project.write_css(std::string(kCss) + ".card { color: blue; }\n");
    check(run_cli(cli, project.arguments(true), project.log_dir) == 1,
          "--check reports a stale embedded blob");
    check(run_cli(cli, project.arguments(false), project.log_dir) == 0,
          "the edited CSS regenerates");
    check(read_file(header_path) != header, "the edited CSS changes the embedded bytes");

    std::error_code error;
    fs::remove_all(project.work, error);
}

void test_cli_rejects_broken_css(const fs::path& cli) {
    TempProject project(false);
    project.write_css(".card { content: \"unterminated;\n}\n");
    check(run_cli(cli, project.arguments(false), project.log_dir) == 1,
          "a syntactically broken stylesheet exits 1");
    const std::string log = read_file(project.log_dir / "cli.log");
    check(log.find("app.css") != std::string::npos, "the diagnostic names the stylesheet");
    check(log.find("TESSERA_CSS_SYNTAX") != std::string::npos,
          "the diagnostic carries the CSS code");
    check(!fs::exists(project.out / "demo_stylesheets.hpp"),
          "a broken stylesheet produces no blob header");

    std::error_code error;
    fs::remove_all(project.work, error);
}

#endif // TESSERA_EMBED_TESTS_NO_SUBPROCESS

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
#if !defined(TESSERA_EMBED_TESTS_NO_SUBPROCESS)
    if (argc != 2) {
        std::cerr << "usage: codegen_stylesheet_embed_tests CLI\n";
        return 2;
    }
#endif
    test_embedded_header_is_pinned();
    test_partial_embeds_keep_the_warning();
    test_without_embeds_nothing_changes();
    test_unknown_embed_is_an_error();
    test_determinism_and_module_order();
#if !defined(TESSERA_EMBED_TESTS_NO_SUBPROCESS)
    test_cli_embeds_and_loads(fs::path(argv[1]).make_preferred());
    test_cli_rejects_broken_css(fs::path(argv[1]).make_preferred());
#else
    std::cout << "codegen stylesheet embed CLI tests skipped: subprocess execution is "
                 "unavailable on this platform\n";
#endif
    return finish("codegen stylesheet embed tests");
}
