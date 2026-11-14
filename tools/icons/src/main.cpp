// SPDX-License-Identifier: MIT
//
// `tessera icons add <set>:<name>...` -- the Iconify import CLI (ADR-047 item 8).
//
// Reads pinned sets from disk (never the Iconify API), runs the license gate, converts each
// referenced static icon into the engine's vector asset form, tree-shakes everything unreferenced,
// and writes deterministic generated files plus attribution.
//
// Exit codes: 0 success, 1 license/import/staleness failure, 2 usage or IO.

#include "icon_import.hpp"
#include "icon_license.hpp"

#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace icons = tessera::icons;

void usage(const char* program) {
    std::printf(
        "usage: %s icons add <set>:<name>... --iconify <dir> --out <dir> [options]\n"
        "\n"
        "  <set>:<name>            icon to import; repeat for each referenced icon\n"
        "  --iconify <dir>         directory of pinned Iconify sets (<dir>/<set>/{info.json,"
        "icons.json})\n"
        "  --out <dir>             output directory for the generated vector assets and notices\n"
        "  --references <file>     extra references, one <set>:<name> per line ('#' comments)\n"
        "  --allow-share-alike     accept share-alike sets (CC-BY-SA) explicitly\n"
        "  --monochrome            force every paint to currentColor (text color token)\n"
        "  --check                 verify the output directory is up to date instead of writing\n"
        "\n"
        "The import is offline and deterministic: the pinned directory is the only input and two\n"
        "runs over it produce identical bytes.\n",
        program);
}

struct Arguments {
    std::vector<std::string> references;
    std::string iconify_root;
    std::string output_root;
    std::string references_file;
    bool allow_share_alike = false;
    bool monochrome = false;
    bool check = false;
};

[[nodiscard]] bool next_argument(const char* program, int argc, char** argv, int& index,
                                 std::string& out) {
    if (index + 1 >= argc) {
        std::fprintf(stderr, "tessera icons: %s needs a value\n", argv[index]);
        usage(program);
        return false;
    }
    ++index;
    out = argv[index];
    return true;
}

[[nodiscard]] std::vector<std::string> read_references(const std::string& path) {
    std::vector<std::string> references;
    std::ifstream file(path, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        references.push_back(line.substr(start, line.find_last_not_of(" \t") - start + 1));
    }
    return references;
}

[[nodiscard]] std::size_t sets_imported(const std::vector<icons::ImportedIcon>& imported) {
    std::set<std::string> sets;
    for (const icons::ImportedIcon& icon : imported) {
        sets.insert(icon.prefix);
    }
    return sets.size();
}

} // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    // Accept the documented `tessera icons add ...` form as well as a bare argument list, so the
    // future `tessera` driver can forward its arguments unchanged.
    int index = 1;
    if (index < argc && std::string_view(argv[index]) == "icons") {
        ++index;
        if (index < argc && std::string_view(argv[index]) == "add") {
            ++index;
        }
    }
    for (; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--iconify") {
            if (!next_argument(argv[0], argc, argv, index, arguments.iconify_root)) {
                return 2;
            }
        } else if (argument == "--out") {
            if (!next_argument(argv[0], argc, argv, index, arguments.output_root)) {
                return 2;
            }
        } else if (argument == "--references") {
            if (!next_argument(argv[0], argc, argv, index, arguments.references_file)) {
                return 2;
            }
        } else if (argument == "--allow-share-alike") {
            arguments.allow_share_alike = true;
        } else if (argument == "--monochrome") {
            arguments.monochrome = true;
        } else if (argument == "--check") {
            arguments.check = true;
        } else if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            return 0;
        } else if (!argument.empty() && argument.front() == '-') {
            std::fprintf(stderr, "tessera icons: unknown option %s\n", argv[index]);
            usage(argv[0]);
            return 2;
        } else {
            arguments.references.emplace_back(argument);
        }
    }
    if (arguments.iconify_root.empty() || arguments.output_root.empty()) {
        usage(argv[0]);
        return 2;
    }
    if (!arguments.references_file.empty()) {
        const std::vector<std::string> extra = read_references(arguments.references_file);
        arguments.references.insert(arguments.references.end(), extra.begin(), extra.end());
    }

    icons::ImportRequest request;
    request.iconify_root = arguments.iconify_root;
    request.references = arguments.references;
    request.allow_share_alike = arguments.allow_share_alike;
    request.monochrome = arguments.monochrome;

    const auto result = icons::import_icons(request);
    if (!result) {
        std::fprintf(stderr, "tessera icons: %s\n", result.error().c_str());
        return 1;
    }

    if (arguments.check) {
        const auto checked = icons::check_files(arguments.output_root, result->files);
        if (!checked) {
            std::fprintf(stderr, "tessera icons: %s\n", checked.error().c_str());
            return 1;
        }
        std::printf("tessera icons: %zu icon(s) up to date in %s\n", result->icons.size(),
                    arguments.output_root.c_str());
        return 0;
    }

    const auto written = icons::write_files(arguments.output_root, result->files);
    if (!written) {
        std::fprintf(stderr, "tessera icons: %s\n", written.error().c_str());
        return 1;
    }
    std::printf("tessera icons: imported %zu icon(s) from %zu set(s) into %s\n",
                result->icons.size(), sets_imported(result->icons), arguments.output_root.c_str());
    for (const icons::ImportedIcon& icon : result->icons) {
        std::printf("  %s:%s -> %s\n", icon.prefix.c_str(), icon.name.c_str(), icon.asset.c_str());
    }
    std::printf("tessera icons: attribution written to %s/ICON_LICENSES.txt\n",
                arguments.output_root.c_str());
    return 0;
}
