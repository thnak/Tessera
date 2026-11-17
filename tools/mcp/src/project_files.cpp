// SPDX-License-Identifier: MIT

#include "project_files.hpp"
#include "tessera/harness/json_parse.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace tessera::mcp {
namespace {

namespace fs = std::filesystem;
namespace harness = tessera::harness;

policy::ProjectFileError refusal(std::string_view code, std::string message) {
    return policy::ProjectFileError{std::string(code), std::move(message)};
}

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// True when `target` is `root` or lies under it. Both are canonical, so a `..` or a link cannot
// make a path look inside while pointing outside.
bool inside(const fs::path& root, const fs::path& target) {
    auto root_it = root.begin();
    auto target_it = target.begin();
    for (; root_it != root.end(); ++root_it, ++target_it) {
        if (target_it == target.end() || *root_it != *target_it) {
            return false;
        }
    }
    return true;
}

struct Walk {
    fs::path root;
    fs::path canonical_root;
    std::vector<ListedFile> files;
    // Paths already judged `Other` by the walk (a symbolic link, a pruned directory): the
    // classification below must not promote them by extension.
    std::set<std::string> pinned;
};

std::optional<policy::ProjectFileError> walk(Walk& state, const fs::path& directory,
                                             const std::string& prefix) {
    std::error_code error;
    std::vector<fs::directory_entry> entries;
    for (fs::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        entries.push_back(*it);
    }
    if (error) {
        return refusal(policy::kPathEscapeCode, "a directory of the project cannot be listed");
    }
    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  return a.path().filename().generic_string() <
                         b.path().filename().generic_string();
              });
    for (const fs::directory_entry& entry : entries) {
        const std::string name = entry.path().filename().generic_string();
        const std::string relative = prefix.empty() ? name : prefix + "/" + name;
        if (auto bad = policy::check_project_path(relative)) {
            return bad;
        }
        const fs::file_status link_status = entry.symlink_status(error);
        if (error) {
            return refusal(policy::kPathEscapeCode, "`" + relative + "` cannot be inspected");
        }
        if (fs::is_symlink(link_status)) {
            const fs::path target = fs::weakly_canonical(entry.path(), error);
            if (error || !inside(state.canonical_root, target)) {
                return refusal(policy::kPathEscapeCode,
                               "`" + relative +
                                   "` is a symbolic link that leaves the project root");
            }
            // Inside the root, but never followed: listed and never read.
            state.files.push_back(ListedFile{relative, policy::ProjectFileKind::Other, 0, {}});
            state.pinned.insert(relative);
        } else if (fs::is_directory(link_status)) {
            if (policy::is_pruned_directory(name)) {
                state.files.push_back(ListedFile{relative, policy::ProjectFileKind::Other, 0, {}});
                state.pinned.insert(relative);
            } else if (auto bad = walk(state, entry.path(), relative)) {
                return bad;
            }
        } else if (fs::is_regular_file(link_status)) {
            const std::uintmax_t size = entry.file_size(error);
            state.files.push_back(ListedFile{relative,
                                             policy::ProjectFileKind::Other,
                                             error ? 0 : static_cast<std::uint64_t>(size),
                                             {}});
        }
        if (state.files.size() > policy::kMaxProjectFiles) {
            return refusal(policy::kFileLimitCode, "the project has more than " +
                                                       std::to_string(policy::kMaxProjectFiles) +
                                                       " files");
        }
    }
    return std::nullopt;
}

// `entry`, `assets.theme`, `assets.fonts`, `assets.images` from a tessera.json text. A config that
// does not parse contributes nothing: the compiler reports the broken config itself.
struct ConfigFacts {
    std::string entry;
    std::string theme;
    std::vector<std::string> assets;
};

ConfigFacts read_config(std::string_view text) {
    ConfigFacts facts;
    const auto parsed = harness::parse_json(text);
    if (!parsed || !parsed->is_object()) {
        return facts;
    }
    if (const auto* entry = parsed->find("entry"); entry != nullptr && entry->is_string()) {
        facts.entry = std::string(*entry->as_string());
    }
    const auto* assets = parsed->find("assets");
    if (assets == nullptr || !assets->is_object()) {
        return facts;
    }
    if (const auto* theme = assets->find("theme"); theme != nullptr && theme->is_string()) {
        facts.theme = std::string(*theme->as_string());
    }
    for (const char* key : {"fonts", "images"}) {
        const auto* list = assets->find(key);
        if (list == nullptr || !list->is_array()) {
            continue;
        }
        for (const auto& item : *list->as_array()) {
            if (item.is_string()) {
                facts.assets.emplace_back(*item.as_string());
            }
        }
    }
    return facts;
}

} // namespace

std::expected<ProjectListing, policy::ProjectFileError> list_project(const fs::path& root) {
    std::error_code error;
    Walk state;
    state.root = root;
    state.canonical_root = fs::canonical(root, error);
    if (error) {
        return std::unexpected(refusal(policy::kPathEscapeCode, "the project root cannot be read"));
    }
    if (auto bad = walk(state, root, {})) {
        return std::unexpected(*bad);
    }
    std::sort(state.files.begin(), state.files.end(),
              [](const ListedFile& a, const ListedFile& b) { return a.path < b.path; });

    ConfigFacts facts;
    const auto config = std::find_if(state.files.begin(), state.files.end(),
                                     [](const ListedFile& f) { return f.path == "tessera.json"; });
    if (config != state.files.end() && config->size <= policy::kMaxProjectFileBytes) {
        if (auto text = read_text(root / "tessera.json")) {
            facts = read_config(*text);
        }
    }
    const policy::ProjectFileContext context{facts.theme, facts.assets};
    std::vector<policy::ProjectFileEntry> judged;
    judged.reserve(state.files.size());
    for (ListedFile& file : state.files) {
        if (state.pinned.count(file.path) == 0) {
            file.kind = policy::classify_project_file(file.path, context);
        }
        judged.push_back(policy::ProjectFileEntry{file.path, file.size, file.kind});
    }
    if (auto bad = policy::check_project_limits(judged)) {
        return std::unexpected(*bad);
    }
    for (ListedFile& file : state.files) {
        if (policy::is_readable(file.kind)) {
            if (auto text = read_text(root / fs::path(file.path))) {
                file.digest = policy::project_file_digest(*text);
            }
        }
    }
    return ProjectListing{std::move(state.files), std::move(facts.entry)};
}

std::expected<std::string, policy::ProjectFileError> read_project_file(const fs::path& root,
                                                                       const ListedFile& file) {
    if (!policy::is_readable(file.kind)) {
        return std::unexpected(refusal(policy::kPathEscapeCode,
                                       "`" + file.path + "` is not an accepted project file"));
    }
    std::optional<std::string> text = read_text(root / fs::path(file.path));
    if (!text) {
        return std::unexpected(
            refusal(policy::kPathEscapeCode, "`" + file.path + "` cannot be read"));
    }
    return std::move(*text);
}

} // namespace tessera::mcp
