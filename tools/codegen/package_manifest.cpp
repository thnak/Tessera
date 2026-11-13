// SPDX-License-Identifier: MIT

// Package version check (issue #234); see package_manifest.hpp.

#include "package_manifest.hpp"
#include "tessera/harness/json_parse.hpp"

#include <charconv>
#include <filesystem>
#include <string>
#include <utility>

namespace tessera::compiler::codegen::cli::internal {
namespace {

// One decimal component: digits only, no sign, no leading zero (except "0"), and small enough that
// a comparison cannot overflow.
bool parse_component(std::string_view text, unsigned& out) {
    if (text.empty() || text.size() > 9 || (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

std::string format(const SemVer& version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
           std::to_string(version.patch);
}

std::string format(const VersionRequirement& requirement) {
    return (requirement.caret ? "^" : "") + format(requirement.version);
}

bool fail(Outcome& outcome, std::string_view code, const std::string& text) {
    report(outcome, true, "tessera.json:1:1: " + std::string(code) + ": " + text);
    outcome.exit_code = 1;
    return false;
}

} // namespace

std::optional<SemVer> parse_semver(std::string_view text) {
    const std::size_t first = text.find('.');
    const std::size_t second = first == std::string_view::npos ? first : text.find('.', first + 1);
    if (second == std::string_view::npos || text.find('.', second + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    SemVer version;
    if (!parse_component(text.substr(0, first), version.major) ||
        !parse_component(text.substr(first + 1, second - first - 1), version.minor) ||
        !parse_component(text.substr(second + 1), version.patch)) {
        return std::nullopt;
    }
    return version;
}

std::optional<VersionRequirement> parse_requirement(std::string_view text) {
    VersionRequirement requirement;
    if (text.starts_with('^')) {
        requirement.caret = true;
        text.remove_prefix(1);
    }
    const std::optional<SemVer> version = parse_semver(text);
    if (!version) {
        return std::nullopt;
    }
    requirement.version = *version;
    return requirement;
}

bool satisfies(const SemVer& installed, const VersionRequirement& required) {
    if (!required.caret) {
        return installed == required.version;
    }
    if (installed.major != required.version.major) {
        return false;
    }
    if (installed.minor != required.version.minor) {
        return installed.minor > required.version.minor;
    }
    return installed.patch >= required.version.patch;
}

std::string manifest_path(std::string_view package_root) {
    return std::string(package_root) + "/tessera-package.json";
}

bool check_package_versions(const ProjectIdentity& identity, Outcome& outcome) {
    for (const PackageRootSpec& package : identity.packages) {
        if (package.version.empty()) {
            continue;
        }
        const std::optional<VersionRequirement> required = parse_requirement(package.version);
        if (!required) {
            return fail(outcome, "TESSERA_PACKAGE_VERSION",
                        "package `" + package.name + "` requires `" + package.version +
                            "`, which is not `X.Y.Z` or `^X.Y.Z`");
        }
        const std::string path = manifest_path(package.root);
        const std::optional<std::string> text =
            read_file(identity.root / std::filesystem::path(path));
        if (!text) {
            return fail(outcome, "TESSERA_PACKAGE_MANIFEST",
                        "package `" + package.name + "` requires " + format(*required) +
                            " but its manifest `" + path + "` cannot be read");
        }
        const auto parsed = tessera::harness::parse_json(*text);
        if (!parsed) {
            return fail(outcome, "TESSERA_PACKAGE_MANIFEST",
                        "`" + path + "` is not valid JSON (offset " +
                            std::to_string(parsed.error().offset) + ")");
        }
        const tessera::harness::JsonValue* name_node = parsed->find("name");
        const tessera::harness::JsonValue* version_node = parsed->find("version");
        const std::optional<std::string_view> name =
            name_node != nullptr ? name_node->as_string() : std::nullopt;
        const std::optional<std::string_view> version =
            version_node != nullptr ? version_node->as_string() : std::nullopt;
        if (!name || !version) {
            return fail(outcome, "TESSERA_PACKAGE_MANIFEST",
                        "`" + path + "` needs a string `name` and a string `version`");
        }
        if (*name != package.name) {
            return fail(outcome, "TESSERA_PACKAGE_MANIFEST",
                        "`" + path + "` names package `" + std::string(*name) +
                            "`, but tessera.json maps it as `" + package.name + "`");
        }
        const std::optional<SemVer> installed = parse_semver(*version);
        if (!installed) {
            return fail(outcome, "TESSERA_PACKAGE_MANIFEST",
                        "`" + path + "` version `" + std::string(*version) + "` is not `X.Y.Z`");
        }
        if (!satisfies(*installed, *required)) {
            return fail(outcome, "TESSERA_PACKAGE_VERSION",
                        "package `" + package.name + "` requires " + format(*required) + " but `" +
                            path + "` provides " + format(*installed));
        }
    }
    return true;
}

} // namespace tessera::compiler::codegen::cli::internal
