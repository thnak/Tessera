// SPDX-License-Identifier: MIT

// `tessera new <dir> [--template blank|counter] [--title TEXT]` (issue #77,
// docs/guides/project-generator.md "Quick start").
//
// This is the CLI and IO half: argument parsing, the name checks the project-config schema imposes
// (`app.name` and `app.namespace`, docs/specs/project-config-v1.md section "app"), the refusal to
// overwrite, and the writes. The file bodies are in new_project_templates.cpp.
//
// Two rules shape the whole unit:
//
//   * **Never eat a directory.** If the target exists and holds anything at all, the run stops
//     before the first byte is written and says so. A scaffolding tool that empties a user's tree
//     is worse than one that does not exist, so there is no --force and no merge: a project is
//     created into a new or empty directory. (Nothing is deleted either way, including on a later
//     failure.)
//   * **Nothing is executed and nothing is fetched.** No `git init`, no package manager, no
//   network.
//     The generated README says how to point the project at a Tessera checkout, which is the one
//     manual step (issue #77's "prebuilt toolchain" is a separate, owner-decided artifact).

#include "new_project.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tessera::generator {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage =
    "usage: tessera new <dir> [--template blank|counter] [--title TEXT]\n"
    "\n"
    "  <dir>              project directory to create (must not exist, or be empty)\n"
    "  --template NAME    starter template (default: blank); `tessera new --list-templates`\n"
    "  --title TEXT       window title (default: derived from the directory name)\n"
    "\n"
    "Creates tessera.json, CMakeLists.txt, CMakePresets.json, .clang-format, .gitignore,\n"
    "README.md, ui/App.tsx, ui/app.css, ui/elements.json and src/{main.cpp,App.hpp,App.cpp}.\n"
    "The output is deterministic and nothing is downloaded: the project builds against an\n"
    "installed Tessera SDK or a Tessera checkout (README.md shows both).\n"
    "\n"
    "Exit codes: 0 created, 1 write failure, 2 usage or the directory is not empty.\n";

int usage_error(std::ostream& err, std::string_view message) {
    err << "tessera new: " << message << "\n\n" << kUsage;
    return 2;
}

// `app.name` of docs/specs/project-config-v1.md: `^[a-z][a-z0-9-]*$`.
bool is_valid_app_name(std::string_view name) {
    if (name.empty() || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    for (const char value : name) {
        const bool digit = value >= '0' && value <= '9';
        if (!(digit || (value >= 'a' && value <= 'z') || value == '-')) {
            return false;
        }
    }
    return true;
}

// `app.namespace`: `^[a-z][a-z0-9_]*$`. Derived from app.name, so only `-` has to become `_`.
std::string namespace_for(std::string_view app_name) {
    std::string ns(app_name);
    for (char& value : ns) {
        if (value == '-') {
            value = '_';
        }
    }
    return ns;
}

// "Hello world": the window title when `--title` is absent. ASCII only and locale-independent, so
// two runs on two machines agree.
std::string default_title(std::string_view app_name) {
    std::string title(app_name);
    for (char& value : title) {
        if (value == '-') {
            value = ' ';
        }
    }
    if (!title.empty() && title.front() >= 'a' && title.front() <= 'z') {
        title.front() = static_cast<char>(title.front() - 'a' + 'A');
    }
    return title;
}

// True when `path` exists and holds at least one entry. A missing path and an empty directory both
// mean "safe to scaffold into".
bool holds_entries(const fs::path& path, std::string& why) {
    std::error_code error;
    const bool exists = fs::exists(path, error);
    if (error) {
        why = "cannot inspect '" + path.string() + "': " + error.message();
        return true;
    }
    if (!exists) {
        return false;
    }
    if (!fs::is_directory(path, error)) {
        why = "'" + path.string() + "' exists and is not a directory";
        return true;
    }
    fs::directory_iterator entry(path, error);
    if (error) {
        why = "cannot read '" + path.string() + "': " + error.message();
        return true;
    }
    if (entry != fs::directory_iterator()) {
        why = "'" + path.string() +
              "' is not empty; `tessera new` never overwrites or merges into an existing project "
              "(create the project in a new or empty directory)";
        return true;
    }
    return false;
}

} // namespace

int run_new(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    std::string directory;
    std::string template_name = "blank";
    std::string title;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string_view argument = args[index];
        if (argument == "--help" || argument == "-h") {
            out << kUsage;
            return 0;
        }
        if (argument == "--list-templates") {
            for (const std::string& name : template_names()) {
                out << name << "\n";
            }
            return 0;
        }
        const auto take_value = [&](std::string_view name) -> std::string {
            if (index + 1 < args.size()) {
                ++index;
                return std::string(args[index]);
            }
            err << "tessera new: " << name << " requires a value\n";
            return {};
        };
        if (argument == "--template" || argument == "--title") {
            std::string value = take_value(argument);
            if (value.empty()) {
                return 2;
            }
            if (argument == "--template") {
                template_name = std::move(value);
            } else {
                title = std::move(value);
            }
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            return usage_error(err, "unknown option `" + std::string(argument) + "`");
        }
        if (!directory.empty()) {
            return usage_error(err, "one project directory, not several");
        }
        directory = std::string(argument);
    }

    if (directory.empty()) {
        return usage_error(err, "no project directory");
    }
    const std::vector<std::string> names = template_names();
    bool known = false;
    for (const std::string& name : names) {
        known = known || name == template_name;
    }
    if (!known) {
        std::string available;
        for (const std::string& name : names) {
            available += available.empty() ? "" : ", ";
            available += name;
        }
        return usage_error(err, "unknown template `" + template_name +
                                    "` (available: " + available + ")");
    }

    const fs::path target(directory);
    const std::string app_name = target.filename().string();
    if (!is_valid_app_name(app_name)) {
        return usage_error(err, "'" + app_name +
                                    "' is not a usable project name: app.name must match "
                                    "^[a-z][a-z0-9-]*$ (docs/specs/project-config-v1.md)");
    }

    std::string why;
    if (holds_entries(target, why)) {
        err << "tessera new: " << why << "\n";
        return 2;
    }

    Options options;
    options.app_name = app_name;
    options.ns = namespace_for(app_name);
    options.title = title.empty() ? default_title(app_name) : title;

    std::error_code error;
    fs::create_directories(target, error);
    if (error) {
        err << "tessera new: cannot create '" << target.string() << "': " << error.message()
            << "\n";
        return 1;
    }

    const std::vector<File> files = project_files(template_name, options);
    std::size_t written = 0;
    for (const File& entry : files) {
        const fs::path path = target / fs::path(entry.path);
        if (!path.parent_path().empty()) {
            fs::create_directories(path.parent_path(), error);
            if (error) {
                err << "tessera new: cannot create '" << path.parent_path().string()
                    << "': " << error.message() << "\n";
                return 1;
            }
        }
        // Binary mode and an explicit '\n'-only body: the same bytes on Windows and Linux, so the
        // twice-into-two-directories comparison is a real equality and not a line-ending accident.
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << entry.text;
        if (!stream) {
            err << "tessera new: cannot write '" << path.string() << "'\n";
            return 1;
        }
        ++written;
    }

    out << "tessera new: created " << options.app_name << " (" << template_name << ", " << written
        << " files) in " << directory << "\n";
    out << "\n";
    out << "Next:\n";
    out << "  cd " << directory << "\n";
    out << "  cmake --preset dev -DTESSERA_SOURCE_DIR=<tessera checkout>\n";
    out << "  cmake --build --preset dev\n";
    out << "  ./build/dev/" << options.app_name << " --frames 120\n";
    out << "\n";
    out << "The project needs a Tessera checkout or an installed SDK and a font (--font);\n";
    out << "see README.md. Nothing was downloaded and no repository was initialised.\n";
    return 0;
}

} // namespace tessera::generator
