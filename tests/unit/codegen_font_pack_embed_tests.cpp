// SPDX-License-Identifier: MIT

// `assets.fontPack` (#970, ADR-047 item 4, docs/specs/theme-versions-v1.md section 5): the codegen
// CLI checks the theme asset's `fontPacks` pins against the named font pack manifest, like
// `assets.themePins`. A matching version embeds, a pack bump the theme did not take fails with
// `TESSERA_FONT_PACK_PIN`, an unreadable manifest with `TESSERA_FONT_PACK_MANIFEST`, and an app
// that names no pack is not checked. Deterministic: literal bytes and a temp project; needs
// subprocess execution, so it is a no-op where that is unavailable.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#if !defined(_WIN32) && !defined(TESSERA_NO_SUBPROCESS)
#include <sys/wait.h>
#endif

namespace {

namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "codegen_font_pack_embed: " << message << "\n";
        ++failures;
    }
}

#if !defined(TESSERA_NO_SUBPROCESS)

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

int run_cli(const fs::path& cli, const std::string& arguments, const fs::path& log_dir) {
    const fs::path log = log_dir / "cli.log";
#ifdef _WIN32
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

std::string manifest(std::string_view version) {
    return R"({"schemaVersion":1,"id":"demo-pack","version":")" + std::string(version) +
           R"(","fonts":[{"file":"a.ttf","family":"A","weight":400,"style":"normal",)" +
           R"("sha256":")" + std::string(64, 'a') + R"(","bytes":1}]})";
}

constexpr std::string_view kTheme = R"({"schemaVersion":1,"themeVersions":{"light":"1.0.0"},)"
                                    R"("fontPacks":{"light":{"id":"demo-pack","version":"2.0.0"}},)"
                                    R"("themes":{"light":{"--bg":"#ffffff"}}})";

struct TempProject {
    fs::path work;
    fs::path project;
    fs::path out;
    fs::path log_dir;

    // `font_pack` is the `assets.fontPack` JSON member including its leading comma, or empty.
    TempProject(std::string_view font_pack, bool make_output) {
        work = fs::temp_directory_path() / "tessera-codegen-font-pack-embed";
        project = work / "project";
        out = work / "out";
        log_dir = work / "logs";
        std::error_code error;
        fs::remove_all(work, error);
        fs::create_directories(project / "ui");
        fs::create_directories(project / "assets");
        fs::create_directories(log_dir);
        if (make_output) {
            fs::create_directories(out);
        }
        write_file(project / "tessera.json",
                   std::string(R"({"schemaVersion":1,"app":{"namespace":"factory"},"assets":)") +
                       R"({"theme":"assets/theme.json")" + std::string(font_pack) + "}}");
        write_file(project / "assets" / "theme.json", kTheme);
        write_file(project / "ui" / "App.tsx", "function App() { return <View />; }\n");
    }

    ~TempProject() {
        std::error_code error;
        fs::remove_all(work, error);
    }

    [[nodiscard]] std::string arguments() const {
        return "--write --out " + quoted(out) + " --namespace demo " +
               quoted(project / "ui" / "App.tsx");
    }
};

constexpr std::string_view kPackMember = R"(,"fontPack":"assets/pack.json")";

void test_no_pack_is_not_checked(const fs::path& cli) {
    TempProject project("", true);
    check(run_cli(cli, project.arguments(), project.log_dir) == 0,
          "an app that names no font pack is not checked against the theme's pins");
}

void test_unsatisfied_pin(const fs::path& cli) {
    TempProject project(kPackMember, false);
    write_file(project.project / "assets" / "pack.json", manifest("1.0.0"));
    check(run_cli(cli, project.arguments(), project.log_dir) == 1,
          "a font pack pin the pack does not satisfy exits 1");
    const std::string log = read_file(project.log_dir / "cli.log");
    check(log.find("TESSERA_FONT_PACK_PIN") != std::string::npos,
          "the pin failure carries its typed code");
    check(!fs::exists(project.out / "demo_theme.hpp"), "a failed pin produces no theme header");
}

void test_satisfied_pin(const fs::path& cli) {
    TempProject project(kPackMember, true);
    write_file(project.project / "assets" / "pack.json", manifest("2.0.0"));
    check(run_cli(cli, project.arguments(), project.log_dir) == 0,
          "a font pack at the pinned version regenerates");
    check(fs::exists(project.out / "demo_theme.hpp"), "the theme still embeds");
}

void test_unreadable_manifest(const fs::path& cli) {
    TempProject project(R"(,"fontPack":"assets/missing.json")", false);
    check(run_cli(cli, project.arguments(), project.log_dir) == 1,
          "an unreadable font pack exits 1");
    check(read_file(project.log_dir / "cli.log").find("TESSERA_FONT_PACK_MANIFEST") !=
              std::string::npos,
          "the unreadable pack is a manifest error");
}

#endif // TESSERA_NO_SUBPROCESS

} // namespace

int main(int argc, char** argv) {
#if !defined(TESSERA_NO_SUBPROCESS)
    if (argc != 2) {
        std::cerr << "usage: codegen_font_pack_embed_tests CLI\n";
        return 2;
    }
    const fs::path cli = fs::path(argv[1]).make_preferred();
    test_no_pack_is_not_checked(cli);
    test_unsatisfied_pin(cli);
    test_satisfied_pin(cli);
    test_unreadable_manifest(cli);
#else
    (void)argc;
    (void)argv;
    std::cout << "codegen font pack embed CLI tests skipped: subprocess execution is unavailable\n";
#endif
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen font pack embed: ok\n";
    return 0;
}
