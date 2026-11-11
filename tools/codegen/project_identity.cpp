// SPDX-License-Identifier: MIT

// Project identity and import resolution for the codegen CLI (docs/specs/project-config-v1.md,
// docs/specs/automation-ids-v1.md section 2, issue #70).

#include "project_identity.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/harness/json_parse.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli::internal {
namespace {

namespace fs = std::filesystem;
namespace lowering = tessera::compiler::lowering;

// Reads the optional `packages` array: every entry is `{ "name": string, "root": string }` with a
// unique name and a project-relative directory (no absolute path, drive letter or `..` escape).
// Returns false and sets `why` on a malformed entry; an absent/null `packages` is valid.
bool read_packages(const tessera::harness::JsonValue& config, ProjectIdentity& identity,
                   std::string& why) {
    const tessera::harness::JsonValue* packages = config.find("packages");
    if (packages == nullptr || packages->is_null()) {
        return true;
    }
    const tessera::harness::JsonValue::Array* entries = packages->as_array();
    if (entries == nullptr) {
        why = "`packages` must be an array of { \"name\", \"root\" } objects";
        return false;
    }
    for (const tessera::harness::JsonValue& entry : *entries) {
        const tessera::harness::JsonValue* name_node = entry.find("name");
        const tessera::harness::JsonValue* root_node = entry.find("root");
        const std::optional<std::string_view> name =
            name_node != nullptr ? name_node->as_string() : std::nullopt;
        const std::optional<std::string_view> root_value =
            root_node != nullptr ? root_node->as_string() : std::nullopt;
        if (!name || name->empty() || !root_value || root_value->empty()) {
            why = "every `packages` entry needs a non-empty `name` and `root`";
            return false;
        }
        if (name->find('/') != std::string_view::npos ||
            name->find('\\') != std::string_view::npos) {
            why = "package name `" + std::string(*name) + "` must not contain a path separator";
            return false;
        }
        std::string root(*root_value);
        std::replace(root.begin(), root.end(), '\\', '/');
        while (root.size() > 1 && root.back() == '/') {
            root.pop_back();
        }
        const bool absolute = root.front() == '/' || (root.size() > 1 && root[1] == ':');
        const bool escapes = root == ".." || root == "." || root.starts_with("../") ||
                             root.find("/../") != std::string::npos || root.ends_with("/..");
        if (absolute || escapes) {
            why = "package root `" + root + "` must be a project-relative directory";
            return false;
        }
        for (const PackageRootSpec& existing : identity.packages) {
            if (existing.name == *name) {
                why = "duplicate package name `" + std::string(*name) + "`";
                return false;
            }
        }
        identity.packages.push_back(PackageRootSpec{std::string(*name), std::move(root)});
    }
    return true;
}

std::optional<fs::path> find_config(const fs::path& start) {
    for (fs::path dir = start;; dir = dir.parent_path()) {
        std::error_code error;
        if (fs::is_regular_file(dir / "tessera.json", error)) {
            return dir / "tessera.json";
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            return std::nullopt;
        }
    }
}

} // namespace

void report(Outcome& outcome, bool is_error, std::string text) {
    outcome.messages.push_back(Message{is_error, std::move(text)});
    if (is_error) {
        outcome.exit_code = outcome.exit_code == 0 ? 1 : outcome.exit_code;
    }
}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::optional<ProjectIdentity> resolve_identity(const Options& options, Outcome& outcome) {
    const fs::path first = fs::absolute(fs::path(options.inputs.front())).parent_path();
    const std::optional<fs::path> config =
        find_config(options.root.empty() ? first : fs::absolute(fs::path(options.root)));
    ProjectIdentity identity;
    identity.salt = options.salt;
    if (!options.root.empty()) {
        identity.root = fs::absolute(fs::path(options.root));
    } else {
        identity.root = config ? config->parent_path() : first;
    }
    if (!identity.salt.empty() || !config) {
        // A config can still declare package roots without providing the salt, so the file is read
        // below whenever it exists.
        if (!config) {
            return identity;
        }
    }
    const auto fail = [&](const std::string& why) {
        report(outcome, true, "tessera_codegen: " + config->generic_string() + ": " + why);
        outcome.exit_code = 1;
        return std::optional<ProjectIdentity>();
    };
    const std::optional<std::string> text = read_file(*config);
    if (!text) {
        return fail("cannot read the file");
    }
    const auto parsed = tessera::harness::parse_json(*text);
    if (!parsed) {
        return fail("not valid JSON (offset " + std::to_string(parsed.error().offset) + ")");
    }
    std::string package_error;
    if (!read_packages(*parsed, identity, package_error)) {
        return fail(package_error);
    }
    if (!identity.salt.empty()) {
        return identity;
    }
    const tessera::harness::JsonValue* app = parsed->find("app");
    const tessera::harness::JsonValue* name = app != nullptr ? app->find("namespace") : nullptr;
    const std::optional<std::string_view> value =
        name != nullptr ? name->as_string() : std::nullopt;
    if (!value || value->empty()) {
        return fail(
            "`app.namespace` is required and must be a non-empty string: it is the id salt");
    }
    identity.salt = std::string(*value);
    return identity;
}

std::optional<std::string> import_target(std::string_view from_file, std::string_view specifier,
                                         const ProjectIdentity& identity) {
    if (specifier == "tessera" || specifier == "react") {
        return std::nullopt;
    }
    if (specifier.starts_with("./") || specifier.starts_with("../")) {
        const std::size_t slash = from_file.rfind('/');
        const std::string_view base =
            slash == std::string_view::npos ? std::string_view() : from_file.substr(0, slash);
        return lowering::join_project_path(base, specifier) + ".tsx";
    }
    for (const PackageRootSpec& package : identity.packages) {
        if (specifier == package.name) {
            return lowering::join_project_path(package.root, "index") + ".tsx";
        }
        if (specifier.size() > package.name.size() &&
            specifier.compare(0, package.name.size(), package.name) == 0 &&
            specifier[package.name.size()] == '/') {
            return lowering::join_project_path(package.root,
                                               specifier.substr(package.name.size() + 1)) +
                   ".tsx";
        }
    }
    return std::nullopt;
}

} // namespace tessera::compiler::codegen::cli::internal
