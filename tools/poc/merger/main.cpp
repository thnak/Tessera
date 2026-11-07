#include <tessera/compiler/merger.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open: " + path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void write_file(const fs::path& path, std::string_view text) {
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write: " + path.string());
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void usage() {
    std::cerr << "usage: merger_poc --generated <file> --out <file> [--existing <file>]\n"
              << "\n"
              << "  --generated  fresh C++ produced by the UI compiler\n"
              << "  --existing   current C++ file (developer logic is preserved)\n"
              << "  --out        merged output file\n";
}

} // namespace

int main(int argc, char** argv) {
    fs::path generated_path;
    fs::path existing_path;
    fs::path output_path;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--generated" && i + 1 < argc) {
            generated_path = argv[++i];
        } else if (arg == "--existing" && i + 1 < argc) {
            existing_path = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            usage();
            return 2;
        }
    }

    if (generated_path.empty() || output_path.empty()) {
        usage();
        return 2;
    }

    try {
        const std::string generated = read_file(generated_path);
        std::string existing;
        if (!existing_path.empty() && fs::exists(existing_path)) {
            existing = read_file(existing_path);
        }

        const tessera::compiler::MergeResult result =
            tessera::compiler::merge(generated, existing);

        for (const std::string& warning : result.warnings) {
            std::cerr << "warning: " << warning << "\n";
        }

        write_file(output_path, result.text);
        std::cout << "wrote " << output_path.string() << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
