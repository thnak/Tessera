// SPDX-License-Identifier: MIT
//
// `tessera new` (issue #77): the scaffolder's contract, proved without a compiler and without the
// network.
//
// The load-bearing assertions are the determinism ones. A generator that stamps a timestamp, an
// absolute path or a random id into its output still "works" on the machine that wrote it and rots
// everywhere else, so the test scaffolds the same project twice into two directories and compares
// every file byte-for-byte, then checks the whole tree for the things that break portability.
//
// Everything here runs in-process against `generator::run_new` and the real filesystem under the
// build tree (never %TEMP%: a confined sandbox denies a child process writing there).

#include "new_project.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

struct Outcome {
    int code = 0;
    std::string out;
    std::string err;
};

// Builds the `string_view` argv of one run from owning strings, so no view outlives its text.
Outcome run(const std::vector<std::string>& args) {
    std::vector<std::string_view> views;
    views.reserve(args.size());
    for (const std::string& argument : args) {
        views.emplace_back(argument);
    }
    std::ostringstream out;
    std::ostringstream err;
    Outcome outcome;
    outcome.code = tessera::generator::run_new(views, out, err);
    outcome.out = out.str();
    outcome.err = err.str();
    return outcome;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every regular file below `root`, as sorted POSIX-relative paths. Empty when `root` is missing.
std::vector<std::string> tree(const fs::path& root) {
    std::vector<std::string> paths;
    if (!fs::is_directory(root)) {
        return paths;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            paths.push_back(fs::relative(entry.path(), root).generic_string());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

void reset(const fs::path& path) {
    std::error_code error;
    fs::remove_all(path, error);
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

bool has_file(const std::vector<std::string>& paths, std::string_view path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

// The scratch root: `argv[1]`, which CMake points at the build tree. Never the source tree and
// never %TEMP% (a confined sandbox denies a write there, and a failed run must stay inspectable).
fs::path g_work_root = fs::current_path() / "new_project_work";

const fs::path& work_root() {
    return g_work_root;
} // A fresh project directory under the build tree, with its parents created and the leaf absent.
fs::path scratch(std::string_view relative) {
    const fs::path path = work_root() / fs::path(relative);
    reset(path);
    fs::create_directories(path.parent_path());
    return path;
}

// The documented layout of docs/guides/project-generator.md, restricted to what a scaffold can
// legitimately create: no `generated/` (build output) and no assets (licensed third-party files).
void check_layout(const fs::path& project, std::string_view label) {
    const std::vector<std::string> paths = tree(project);
    const std::string where = std::string(label) + ": ";
    check(has_file(paths, "tessera.json"), where + "tessera.json");
    check(has_file(paths, "CMakeLists.txt"), where + "CMakeLists.txt");
    check(has_file(paths, "CMakePresets.json"), where + "CMakePresets.json");
    check(has_file(paths, ".clang-format"), where + ".clang-format");
    check(has_file(paths, ".gitignore"), where + ".gitignore");
    check(has_file(paths, "README.md"), where + "README.md");
    check(has_file(paths, "src/main.cpp"), where + "src/main.cpp");
    check(has_file(paths, "src/App.hpp"), where + "src/App.hpp");
    check(has_file(paths, "src/App.cpp"), where + "src/App.cpp");
    check(has_file(paths, "ui/App.tsx"), where + "ui/App.tsx");
    check(has_file(paths, "ui/app.css"), where + "ui/app.css");
    check(has_file(paths, "ui/elements.json"), where + "ui/elements.json");
    check(!has_file(paths, "generated/App.g.hpp"), where + "no generated base is committed");
    check(!has_file(paths, ".git"), where + "no git repository initialised");
}

// The app and the framework must agree on the C/C++ runtime (ADR-012), and the framework's own
// setting is directory-scoped, so the project sets it before the framework is added. Without it
// the app links /MD objects against /MT framework libraries: LNK2038 after the whole build.
void check_runtime_library(const fs::path& project) {
    const std::string text = read_file(project / "CMakeLists.txt");
    const std::size_t runtime = text.find("set(CMAKE_MSVC_RUNTIME_LIBRARY");
    const std::size_t framework = text.find("add_subdirectory(${TESSERA_SOURCE_DIR}");
    check(runtime != std::string::npos, "CMakeLists.txt: the MSVC runtime library is set");
    check(framework != std::string::npos, "CMakeLists.txt: the framework is added");
    check(runtime < framework,
          "CMakeLists.txt: the runtime is chosen before the framework is added");
    check(contains(text, "MultiThreaded$<$<CONFIG:Debug>:Debug>"),
          "CMakeLists.txt: the static runtime, the framework's own value");
}

// The entry point hands the generated `Path` assets to the window and to the capture, and builds
// for a module with no `Path` node (no `<ns>_vector_assets.hpp`) because the include is
// conditional.
void check_entry_point(const fs::path& project, std::string_view ns) {
    const std::string text = read_file(project / "src/main.cpp");
    const std::string header = std::string(ns) + "_vector_assets.hpp";
    check(contains(text, "#if __has_include(\"" + header + "\")"),
          "main.cpp: the vector assets header is a conditional include");
    check(contains(text, std::string(ns) + "::gen::vector_assets::bundle()"),
          "main.cpp: the generated bundle is taken from the module namespace");
    check(contains(text, "options.vector_assets = &vector_bundle;"),
          "main.cpp: the window gets the bundle");
    check(contains(text, "shot.vector_assets = options.vector_assets;"),
          "main.cpp: the capture gets the same bundle");
}

void check_manifest(const fs::path& project, std::string_view app_name, std::string_view ns,
                    bool expects_state) {
    const std::string text = read_file(project / "tessera.json");
    check(contains(text, "\"schemaVersion\": 1"), "tessera.json: schemaVersion 1");
    check(contains(text, "\"name\": \"" + std::string(app_name) + "\""), "tessera.json: app.name");
    check(contains(text, "\"namespace\": \"" + std::string(ns) + "\""), "tessera.json: namespace");
    check(contains(text, "\"entry\": \"ui/App.tsx\""), "tessera.json: entry names the TSX");
    check(contains(text, "\"sourceDirs\": [\"ui\"]"), "tessera.json: sourceDirs");
    check(contains(text, "\"outDir\": \"generated\""), "tessera.json: the spec default outDir");
    // The hook allowlist is the template's real hook set, so the compiler rejects a hook the
    // template never opted into instead of accepting it by omission.
    check(contains(text, "\"allowed\": [\"useState\"]") == expects_state,
          "tessera.json: the template's hook allowlist");
}

// What a reviewer would check by hand: no absolute path, no timestamp, no host separator, LF only.
//
// The separator check is about *paths*, so it looks at the generated file names for a backslash in
// every file, and at file *content* only outside C++ sources: a `.cpp` legitimately contains `\n`
// and `\"` escapes, and rejecting those would prove nothing about portability.
void check_portable(const fs::path& project) {
    for (const std::string& path : tree(project)) {
        check(path.find('\\') == std::string::npos, path + ": POSIX path separators only");
        const std::string text = read_file(project / path);
        const std::string where = path + ": ";
        check(text.find("C:\\") == std::string::npos, where + "no Windows absolute path");
        check(text.find("/home/") == std::string::npos, where + "no POSIX home path");
        check(text.find('\r') == std::string::npos, where + "LF line endings only");
        check(text.find("2026") == std::string::npos, where + "no timestamp");
        const bool cxx = path.ends_with(".cpp") || path.ends_with(".hpp");
        if (!cxx) {
            check(text.find('\\') == std::string::npos, where + "no backslash outside C++ escapes");
        }
    }
    // The source trees the compiler will see carry POSIX include paths, not host paths.
    check(read_file(project / "src/App.hpp").find("#include \"App.g.hpp\"") != std::string::npos,
          "src/App.hpp includes the generated base by bare name");
}

// Scaffold twice and compare. This is the assertion the whole design exists to satisfy.
void check_deterministic() {
    const fs::path first = scratch("one/hello-world");
    const fs::path second = scratch("two/hello-world");

    const Outcome left = run({first.string()});
    const Outcome right = run({second.string()});
    check(left.code == 0, "first scaffold succeeded: " + left.err);
    check(right.code == 0, "second scaffold succeeded: " + right.err);

    const std::vector<std::string> left_paths = tree(first);
    const std::vector<std::string> right_paths = tree(second);
    check(!left_paths.empty(), "the scaffold produced files");
    check(left_paths == right_paths, "the two trees hold the same files");
    for (const std::string& path : left_paths) {
        const std::string left_text = read_file(first / path);
        const std::string right_text = read_file(second / path);
        check(!left_text.empty(), "non-empty: " + path);
        check(left_text == right_text, "byte-identical: " + path);
    }
}

void check_refuses_non_empty() {
    const fs::path root = scratch("occupied");
    fs::create_directories(root);
    const fs::path keep = root / "notes.txt";
    {
        std::ofstream out(keep, std::ios::binary | std::ios::trunc);
        out << "a file the user already had\n";
    }

    const Outcome outcome = run({root.string()});
    check(outcome.code == 2, "a non-empty directory is a usage error (exit 2)");
    check(contains(outcome.err, "not empty"), "the refusal says the directory is not empty");
    check(fs::exists(keep), "the user's existing file survived");
    check(read_file(keep) == "a file the user already had\n", "the user's file is untouched");
    check(!fs::exists(root / "tessera.json"), "nothing was scaffolded into it");
}

void check_empty_directory_is_accepted() {
    const fs::path root = scratch("empty-target");
    fs::create_directories(root);
    const Outcome outcome = run({root.string()});
    check(outcome.code == 0, "an existing empty directory is scaffolded into: " + outcome.err);
    check(fs::exists(root / "tessera.json"), "the empty directory received the project");
}

void check_templates_differ() {
    const fs::path blank = scratch("tpl-blank/demo");
    const fs::path counter = scratch("tpl-counter/demo");
    const Outcome left = run({blank.string(), "--template", "blank"});
    const Outcome right = run({counter.string(), "--template", "counter"});
    check(left.code == 0, "blank scaffolded: " + left.err);
    check(right.code == 0, "counter scaffolded: " + right.err);
    check(read_file(blank / "ui/App.tsx") != read_file(counter / "ui/App.tsx"),
          "--template changes the UI source");
    check(read_file(blank / "tessera.json") != read_file(counter / "tessera.json"),
          "--template changes the hook allowlist");
    check(contains(read_file(counter / "ui/App.tsx"), "useState<int>"),
          "the counter template holds state");
    check(!contains(read_file(blank / "ui/App.tsx"), "useState"), "the blank template holds none");
    check_manifest(blank, "demo", "demo", false);
    check_manifest(counter, "demo", "demo", true);
    check_entry_point(blank, "demo");
    check_entry_point(counter, "demo");
    check_runtime_library(blank);
    check_runtime_library(counter);
    check_layout(counter, "counter layout");
}

void check_usage() {
    check(run({}).code == 2, "no directory is a usage error");
    check(run({"x-app", "--template", "nope"}).code == 2, "an unknown template is a usage error");
    const Outcome bad = run({"Bad Name"});
    check(bad.code == 2, "an unusable app name is a usage error");
    check(contains(bad.err, "app.name"), "the error names the schema field");

    // A name with a `-` must produce a valid C++ namespace, not a `-` in an identifier.
    const fs::path dashed = scratch("dashed/hello-world");
    const Outcome outcome = run({dashed.string()});
    check(outcome.code == 0, "a dashed name is scaffolded: " + outcome.err);
    check_manifest(dashed, "hello-world", "hello_world", false);
    check(contains(read_file(dashed / "src/App.hpp"), "namespace hello_world"),
          "the developer class lives in the dashed name's namespace");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        g_work_root = fs::path(argv[1]);
    }
    std::error_code error;
    fs::remove_all(work_root(), error);
    fs::create_directories(work_root(), error);

    check_deterministic();
    check_refuses_non_empty();
    check_empty_directory_is_accepted();
    check_templates_differ();
    check_usage();

    const fs::path blank = work_root() / "one" / "hello-world";
    check_layout(blank, "blank layout");
    check_manifest(blank, "hello-world", "hello_world", false);
    check_portable(blank);

    if (failures != 0) {
        std::cerr << "new_project: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "new_project: ok\n";
    return 0;
}
