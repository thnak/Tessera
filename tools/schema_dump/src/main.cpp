// SPDX-License-Identifier: MIT

// tessera_schema_dump: writes intrinsics.json and tessera.d.ts from the shared schema
// (docs/specs/jsx-intrinsics-v1/machine-readable.md section 4). The golden test uses --check; a
// deliberate schema change uses --write.

#include "core.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void usage() {
    std::cout << "usage: tessera_schema_dump [--json [path]] [--dts [path]]\n"
                 "       tessera_schema_dump --check <dir>\n"
                 "       tessera_schema_dump --write <dir>\n"
                 "\n"
                 "  --json [path]   write intrinsics.json (stdout when no path)\n"
                 "  --dts [path]    write tessera.d.ts (stdout when no path)\n"
                 "  --check <dir>   compare <dir>/intrinsics.json and <dir>/tessera.d.ts\n"
                 "  --write <dir>   update both files (a deliberate schema change)\n";
}

bool write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        std::cerr << "error: cannot write " << path.string() << "\n";
        return false;
    }
    return true;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

bool compare(const std::filesystem::path& path, std::string_view expected) {
    const std::string actual = read_file(path);
    if (actual == expected) {
        return true;
    }
    std::cerr << "error: " << path.string()
              << " is out of date; run tessera_schema_dump --write tests/golden/schema\n";
    return false;
}

} // namespace

int main(int argc, char** argv) {
    bool ok = true;
    for (int i = 1; i < argc && ok; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            usage();
            return 0;
        }
        if (argument == "--json" || argument == "--dts") {
            const bool json = argument == "--json";
            const std::string text =
                json ? tessera::schema::dump::json() : tessera::schema::dump::dts();
            if (i + 1 < argc && std::string_view(argv[i + 1]).rfind("--", 0) != 0) {
                ok = write_file(argv[++i], text);
            } else {
                std::cout << text;
            }
            continue;
        }
        if (argument == "--check" || argument == "--write") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << argument << " needs a directory\n";
                return 2;
            }
            const std::filesystem::path directory = argv[++i];
            const std::string json = tessera::schema::dump::json();
            const std::string dts = tessera::schema::dump::dts();
            const std::filesystem::path json_path = directory / "intrinsics.json";
            const std::filesystem::path dts_path = directory / "tessera.d.ts";
            if (argument == "--check") {
                ok = compare(json_path, json) && compare(dts_path, dts);
            } else {
                std::error_code error;
                std::filesystem::create_directories(directory, error);
                ok = write_file(json_path, json) && write_file(dts_path, dts);
                if (ok) {
                    std::cout << "wrote " << json_path.string() << " and " << dts_path.string()
                              << "\n";
                }
            }
            continue;
        }
        std::cerr << "error: unknown argument " << argument << "\n";
        usage();
        return 2;
    }
    return ok ? 0 : 1;
}
