// SPDX-License-Identifier: MIT

#include "icon_emit.hpp"
#include "icon_import.hpp"
#include "icon_license.hpp"
#include "icon_set.hpp"
#include "icon_svg.hpp"
#include "icon_types.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::icons {
namespace {

namespace fs = std::filesystem;

struct Reference {
    std::string prefix;
    std::string name;
};

[[nodiscard]] std::expected<Reference, std::string> parse_reference(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
        return std::unexpected("reference \"" + std::string(text) + "\" is not <set>:<name>");
    }
    Reference reference;
    reference.prefix = std::string(text.substr(0, colon));
    reference.name = std::string(text.substr(colon + 1));
    return reference;
}

[[nodiscard]] std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace

std::expected<ImportResult, std::string> import_icons(const ImportRequest& request) {
    if (request.references.empty()) {
        return std::unexpected("no icons referenced; pass <set>:<name> or --references <file>");
    }
    if (request.iconify_root.empty()) {
        return std::unexpected("no pinned Iconify directory given; pass --iconify <dir>");
    }
    // Group references by set so each set is loaded and license-checked exactly once. A std::map
    // also makes the iteration order -- and therefore the output -- independent of the argument
    // order.
    std::map<std::string, std::set<std::string>> wanted;
    for (const std::string& text : request.references) {
        const auto reference = parse_reference(text);
        if (!reference) {
            return std::unexpected(reference.error());
        }
        wanted[reference->prefix].insert(reference->name);
    }

    ImportResult result;
    std::vector<AttributionSet> attribution;
    for (const auto& [prefix, names] : wanted) {
        const auto set = load_icon_set(request.iconify_root, prefix);
        if (!set) {
            return std::unexpected(set.error());
        }
        const std::string license = declared_license(*set);
        const LicenseGate gate = check_license(prefix, license, request.allow_share_alike);
        if (!gate.accepted) {
            return std::unexpected(license_rejection_message(prefix, license, gate));
        }
        AttributionSet entry;
        entry.prefix = prefix;
        entry.display_name = set->display_name;
        entry.author = set->author;
        entry.author_url = set->author_url;
        entry.license = license;
        entry.license_url = set->license_url;
        entry.license_class = gate.license.license_class;
        for (const std::string& name : names) {
            const IconSource* const icon = set->find(name);
            if (icon == nullptr) {
                return std::unexpected("icon set \"" + prefix + "\" has no icon \"" + name + "\"");
            }
            const auto svg = canonical_icon_svg(*icon, SvgOptions{request.monochrome});
            if (!svg) {
                return std::unexpected(prefix + ":" + name + ": " + svg.error());
            }
            ImportedIcon imported;
            imported.prefix = prefix;
            imported.name = name;
            imported.asset = "icons/" + prefix + "/" + name + ".svg";
            imported.width = icon->width == 0 ? set->width : icon->width;
            imported.height = icon->height == 0 ? set->height : icon->height;
            imported.monochrome = request.monochrome;
            result.icons.push_back(imported);
            entry.icons.push_back(name);
            result.files.push_back(EmittedFile{imported.asset, *svg});
        }
        attribution.push_back(std::move(entry));
    }

    std::sort(result.icons.begin(), result.icons.end(),
              [](const ImportedIcon& lhs, const ImportedIcon& rhs) {
                  return lhs.prefix != rhs.prefix ? lhs.prefix < rhs.prefix : lhs.name < rhs.name;
              });
    result.files.push_back(EmittedFile{"icons.manifest", build_embed_manifest(result)});
    result.files.push_back(EmittedFile{"icons.g.hpp", build_icons_header(result)});
    result.files.push_back(EmittedFile{"ICON_LICENSES.txt", build_attribution(attribution)});
    std::sort(result.files.begin(), result.files.end(),
              [](const EmittedFile& lhs, const EmittedFile& rhs) { return lhs.path < rhs.path; });
    return result;
}

std::expected<void, std::string> write_files(const std::string& root,
                                             const std::vector<EmittedFile>& files) {
    std::error_code error;
    for (const EmittedFile& file : files) {
        const fs::path target = fs::path(root) / file.path;
        if (!target.parent_path().empty()) {
            fs::create_directories(target.parent_path(), error);
            if (error) {
                return std::unexpected("cannot create " + target.parent_path().string() + ": " +
                                       error.message());
            }
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(file.bytes.data(), static_cast<std::streamsize>(file.bytes.size()));
        }
        if (!out) {
            return std::unexpected("cannot write " + target.string());
        }
    }
    return {};
}

std::expected<void, std::string> check_files(const std::string& root,
                                             const std::vector<EmittedFile>& files) {
    std::set<std::string> expected;
    for (const EmittedFile& file : files) {
        expected.insert(file.path);
    }
    for (const EmittedFile& file : files) {
        const fs::path target = fs::path(root) / file.path;
        const std::optional<std::string> existing = read_file(target);
        if (!existing) {
            return std::unexpected("missing generated file " + target.string());
        }
        if (*existing != file.bytes) {
            return std::unexpected("generated file " + target.string() +
                                   " does not match the pinned sets; rerun tessera icons add");
        }
    }
    // A stale icon from an earlier import is just as wrong as a stale byte, and it would still ship
    // in the embedded bundle, so --check refuses it.
    std::error_code error;
    const fs::path icon_root = fs::path(root) / "icons";
    if (fs::exists(icon_root, error)) {
        for (const fs::directory_entry& entry :
             fs::recursive_directory_iterator(icon_root, error)) {
            if (!entry.is_regular_file(error)) {
                continue;
            }
            const std::string relative =
                "icons/" + fs::relative(entry.path(), icon_root).generic_string();
            if (expected.count(relative) == 0) {
                return std::unexpected("stale generated file " +
                                       (fs::path(root) / relative).string() +
                                       "; rerun tessera icons add without the old output");
            }
        }
    }
    return {};
}

} // namespace tessera::icons
