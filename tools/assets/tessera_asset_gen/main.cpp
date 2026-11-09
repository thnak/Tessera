// SPDX-License-Identifier: MIT
//
// tessera_asset_gen: turns a small manifest plus its files into a deterministic C++ header/source
// pair that embeds the asset bytes in the binary. Output contains no timestamps, no absolute paths
// and no locale-dependent formatting, so the same manifest always produces byte-identical files.

#include "tessera/ui/assets/asset_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace assets = tessera::ui::assets;
namespace fs = std::filesystem;

struct Options {
    fs::path manifest;
    fs::path header;
    fs::path source;
    std::string name_space = "tessera::ui::assets::embedded";
};

struct AssetSource {
    std::string name;
    std::string mime;
    std::vector<std::uint8_t> bytes;
};

struct Error {
    std::string message;
};

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::optional<Error> parse_arguments(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const bool has_value = index + 1 < argc;
        if (argument == "--manifest" && has_value) {
            options.manifest = argv[++index];
        } else if (argument == "--header" && has_value) {
            options.header = argv[++index];
        } else if (argument == "--source" && has_value) {
            options.source = argv[++index];
        } else if (argument == "--namespace" && has_value) {
            options.name_space = argv[++index];
        } else {
            return Error{"unknown or incomplete argument: " + std::string(argument)};
        }
    }
    if (options.manifest.empty() || options.header.empty() || options.source.empty()) {
        return Error{"--manifest, --header and --source are required"};
    }
    if (options.header.parent_path() != options.source.parent_path()) {
        return Error{"--header and --source must be in the same directory"};
    }
    const auto valid_character = [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == ':' || character == '_';
    };
    if (options.name_space.empty() ||
        !std::all_of(options.name_space.begin(), options.name_space.end(), valid_character) ||
        options.name_space.find(' ') != std::string::npos) {
        return Error{"invalid --namespace"};
    }
    return std::nullopt;
}

[[nodiscard]] std::string mime_for(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".txt") {
        return "text/plain";
    }
    if (extension == ".json") {
        return "application/json";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    return "application/octet-stream";
}

[[nodiscard]] std::optional<Error> read_bytes(const fs::path& path,
                                              std::vector<std::uint8_t>& out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Error{"cannot open asset file: " + path.filename().string()};
    }
    out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (!input.eof() && input.fail()) {
        return Error{"cannot read asset file: " + path.filename().string()};
    }
    if (out.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Error{"asset is larger than 4 GiB: " + path.filename().string()};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<Error> load_manifest(const fs::path& manifest,
                                                 std::vector<AssetSource>& assets) {
    std::ifstream input(manifest, std::ios::binary);
    if (!input) {
        return Error{"cannot open manifest"};
    }
    const fs::path directory = manifest.parent_path();
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::string_view trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const std::size_t equals = trimmed.find('=');
        if (equals == std::string_view::npos) {
            return Error{"manifest line " + std::to_string(line_number) + " is not 'name = path'"};
        }
        const std::string_view raw_name = trim(trimmed.substr(0, equals));
        const std::string_view raw_path = trim(trimmed.substr(equals + 1));
        if (raw_name.empty() || raw_path.empty()) {
            return Error{"manifest line " + std::to_string(line_number) +
                         " has an empty name or path"};
        }
        const auto normalized = assets::normalize_asset_name(raw_name);
        if (!normalized.has_value()) {
            return Error{"manifest line " + std::to_string(line_number) +
                         " has an invalid asset name"};
        }
        const fs::path asset_path = fs::path(std::string(raw_path)).is_absolute()
                                        ? fs::path(std::string(raw_path))
                                        : directory / std::string(raw_path);
        AssetSource source;
        source.name = std::string(*normalized);
        source.mime = mime_for(asset_path);
        if (const auto error = read_bytes(asset_path, source.bytes)) {
            return error;
        }
        assets.push_back(std::move(source));
    }
    return std::nullopt;
}

[[nodiscard]] std::string formatted_bytes(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 6 + 32);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index % 12 == 0) {
            text.append("\n    ");
        } else {
            text.push_back(' ');
        }
        text.append("0x");
        text.push_back(kHex[bytes[index] >> 4]);
        text.push_back(kHex[bytes[index] & 0x0F]);
        text.push_back(',');
    }
    text.push_back('\n');
    return text;
}

[[nodiscard]] std::string escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (const auto error = parse_arguments(argc, argv, options)) {
        std::cerr << "tessera_asset_gen: " << error->message << "\n";
        return 2;
    }
    std::vector<AssetSource> assets;
    if (const auto error = load_manifest(options.manifest, assets)) {
        std::cerr << "tessera_asset_gen: " << error->message << "\n";
        return 2;
    }
    std::sort(assets.begin(), assets.end(), [](const AssetSource& left, const AssetSource& right) {
        return left.name < right.name;
    });
    for (std::size_t index = 1; index < assets.size(); ++index) {
        if (assets[index - 1].name == assets[index].name) {
            std::cerr << "tessera_asset_gen: duplicate asset name '" << assets[index].name << "'\n";
            return 2;
        }
    }

    std::size_t total_bytes = 0;
    for (const AssetSource& asset : assets) {
        total_bytes += asset.bytes.size();
    }

    const std::string manifest_name = options.manifest.filename().string();
    const std::string header_name = options.header.filename().string();
    const std::string ns = options.name_space;

    std::string header = "// SPDX-License-Identifier: MIT\n";
    header += "// Generated by tessera_asset_gen from " + manifest_name + "; do not edit.\n";
    header += "// Regenerate with: tessera_asset_gen --manifest <manifest> --namespace " + ns +
              "\n//                  --header <file.hpp> --source <file.cpp>\n\n";
    header += "#pragma once\n\n";
    header += "#include \"tessera/ui/assets/asset_bundle.hpp\"\n\n";
    header += "#include <cstddef>\n\n";
    header += "namespace " + ns + " {\n\n";
    header += "// Number of embedded assets and total payload bytes.\n";
    header += "inline constexpr std::size_t kAssetCount = " + std::to_string(assets.size()) + ";\n";
    header += "inline constexpr std::size_t kAssetBytes = " + std::to_string(total_bytes) + ";\n\n";
    header += "[[nodiscard]] ::tessera::ui::assets::AssetBundle bundle() noexcept;\n\n";
    header += "} // namespace " + ns + "\n";

    std::string source = "// SPDX-License-Identifier: MIT\n";
    source += "// Generated by tessera_asset_gen from " + manifest_name + "; do not edit.\n\n";
    source += "#include \"" + header_name + "\"\n\n";
    source += "#include <cstddef>\n";
    source += "#include <span>\n\n";
    source += "namespace " + ns + " {\nnamespace {\n\n";
    if (assets.empty() || total_bytes == 0) {
        source += "constexpr unsigned char kAssetData[] = {0x00};\n";
    } else {
        source += "constexpr unsigned char kAssetData[] = {";
        std::vector<std::uint8_t> flattened;
        flattened.reserve(total_bytes);
        for (const AssetSource& asset : assets) {
            flattened.insert(flattened.end(), asset.bytes.begin(), asset.bytes.end());
        }
        source += formatted_bytes(flattened);
        source += "};\n";
    }
    source += "\nconstexpr ::tessera::ui::assets::AssetEntry kAssetEntries[] = {\n";
    if (assets.empty()) {
        source += "    {},\n";
    } else {
        std::size_t offset = 0;
        for (const AssetSource& asset : assets) {
            source += "    {\"" + escape(asset.name) + "\", " + std::to_string(offset) + ", " +
                      std::to_string(asset.bytes.size()) + ", \"" + escape(asset.mime) + "\"},\n";
            offset += asset.bytes.size();
        }
    }
    source += "};\n\n} // namespace\n\n";
    source += "::tessera::ui::assets::AssetBundle bundle() noexcept {\n";
    source += "    return ::tessera::ui::assets::AssetBundle(\n";
    source += "        std::span<const ::tessera::ui::assets::AssetEntry>(kAssetEntries, "
              "kAssetCount),\n";
    source += "        std::as_bytes(std::span<const unsigned char>(kAssetData, kAssetBytes)));\n";
    source += "}\n\n} // namespace " + ns + "\n";

    std::error_code error_code;
    if (!options.header.parent_path().empty()) {
        fs::create_directories(options.header.parent_path(), error_code);
        if (error_code) {
            std::cerr << "tessera_asset_gen: cannot create output directory\n";
            return 2;
        }
    }
    write_file(options.header, header);
    write_file(options.source, source);
    std::cout << "tessera_asset_gen: " << assets.size() << " assets, " << total_bytes
              << " bytes -> " << options.header.filename().string() << ", "
              << options.source.filename().string() << "\n";
    return 0;
}
