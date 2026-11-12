// SPDX-License-Identifier: MIT

#include "tessera/automation/app_config.hpp"
#include "tessera/harness/artifact_sink.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_value.hpp"

#include <charconv>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>

namespace tessera::automation {
namespace {

using harness::JsonValue;

std::unexpected<AppConfigError> fail(AppConfigErrorCode code, std::string message) {
    return std::unexpected(AppConfigError{code, std::move(message)});
}

// Decimal text of a non-zero uint64 without sign or leading zeros (automation-ids-v1 section 1).
std::optional<std::uint64_t> parse_id(const JsonValue* value) {
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    const std::string_view text = *value->as_string();
    if (text.empty() || text.size() > 20 || text.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t id = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (ec != std::errc{} || end != text.data() + text.size() || id == 0) {
        return std::nullopt;
    }
    return id;
}

std::optional<std::string> bounded_string(const JsonValue* value, bool allow_empty) {
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    const std::string_view text = *value->as_string();
    if (text.size() > kMaxManifestStringBytes || (!allow_empty && text.empty())) {
        return std::nullopt;
    }
    return std::string(text);
}

std::optional<std::string> read_file(const std::string& path, std::size_t max_bytes,
                                     bool& too_large) {
    too_large = false;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string text;
    char buffer[4096];
    while (file.read(buffer, sizeof buffer) || file.gcount() > 0) {
        text.append(buffer, static_cast<std::size_t>(file.gcount()));
        if (text.size() > max_bytes) {
            too_large = true;
            return std::nullopt;
        }
    }
    return text;
}

std::string parent_directory(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string join(const std::string& directory, std::string_view name) {
    return directory.empty() ? std::string(name) : directory + "/" + std::string(name);
}

} // namespace

std::string_view to_string(AppConfigErrorCode code) noexcept {
    switch (code) {
    case AppConfigErrorCode::ConfigUnreadable:
        return "ConfigUnreadable";
    case AppConfigErrorCode::ConfigTooLarge:
        return "ConfigTooLarge";
    case AppConfigErrorCode::ConfigInvalid:
        return "ConfigInvalid";
    case AppConfigErrorCode::ManifestMissing:
        return "ManifestMissing";
    case AppConfigErrorCode::ManifestTooLarge:
        return "ManifestTooLarge";
    case AppConfigErrorCode::ManifestInvalid:
        return "ManifestInvalid";
    case AppConfigErrorCode::ManifestUnsupportedVersion:
        return "ManifestUnsupportedVersion";
    case AppConfigErrorCode::ManifestTooManyNodes:
        return "ManifestTooManyNodes";
    case AppConfigErrorCode::ManifestDuplicateId:
        return "ManifestDuplicateId";
    }
    return "ConfigInvalid";
}

std::expected<AppManifest, AppConfigError> parse_manifest(std::string_view text) {
    if (text.size() > kMaxManifestBytes) {
        return fail(AppConfigErrorCode::ManifestTooLarge, "manifest exceeds the size cap");
    }
    auto document = harness::parse_json(text);
    if (!document.has_value() || !document->is_object()) {
        return fail(AppConfigErrorCode::ManifestInvalid, "manifest is not a JSON object");
    }
    const JsonValue* version = document->find("manifestVersion");
    if (version == nullptr || !version->as_integer().has_value()) {
        return fail(AppConfigErrorCode::ManifestInvalid, "manifestVersion is required");
    }
    if (*version->as_integer() != kSupportedManifestVersion) {
        return fail(AppConfigErrorCode::ManifestUnsupportedVersion,
                    "unsupported manifestVersion " + std::to_string(*version->as_integer()));
    }
    const JsonValue* components = document->find("components");
    const JsonValue* nodes = document->find("nodes");
    if (components == nullptr || !components->is_array() || nodes == nullptr ||
        !nodes->is_array()) {
        return fail(AppConfigErrorCode::ManifestInvalid, "components and nodes must be arrays");
    }
    if (nodes->size() > kMaxManifestNodes || components->size() > kMaxManifestComponents) {
        return fail(AppConfigErrorCode::ManifestTooManyNodes, "manifest has too many entries");
    }

    AppManifest manifest;
    if (const JsonValue* salt = document->find("projectSalt"); salt != nullptr) {
        auto value = bounded_string(salt, true);
        if (!value.has_value()) {
            return fail(AppConfigErrorCode::ManifestInvalid, "projectSalt must be a string");
        }
        manifest.project_salt = std::move(*value);
    }
    for (const JsonValue& entry : *components->as_array()) {
        const auto id = parse_id(entry.find("componentId"));
        auto name = bounded_string(entry.find("name"), false);
        auto source = bounded_string(entry.find("source"), true);
        if (!entry.is_object() || !id.has_value() || !name.has_value() || !source.has_value()) {
            return fail(AppConfigErrorCode::ManifestInvalid, "malformed component entry");
        }
        manifest.components.push_back({*id, std::move(*name), std::move(*source)});
    }
    std::unordered_set<std::uint64_t> ids;
    std::set<std::string, std::less<>> paths;
    for (const JsonValue& entry : *nodes->as_array()) {
        const auto ui_id = parse_id(entry.find("uiId"));
        const auto component_id = parse_id(entry.find("componentId"));
        auto kind = bounded_string(entry.find("kind"), false);
        auto path = bounded_string(entry.find("path"), false);
        if (!entry.is_object() || !ui_id.has_value() || !component_id.has_value() ||
            !kind.has_value() || !path.has_value()) {
            return fail(AppConfigErrorCode::ManifestInvalid, "malformed node entry");
        }
        if (!ids.insert(*ui_id).second) {
            return fail(AppConfigErrorCode::ManifestDuplicateId,
                        "duplicate uiId " + std::to_string(*ui_id));
        }
        if (!paths.insert(*path).second) {
            return fail(AppConfigErrorCode::ManifestInvalid, "duplicate node path '" + *path + "'");
        }
        ManifestNode node;
        node.ui_id = *ui_id;
        node.component_id = *component_id;
        node.kind = std::move(*kind);
        node.path = std::move(*path);
        if (const JsonValue* source = entry.find("source"); source != nullptr) {
            auto file = bounded_string(source->find("file"), true);
            if (!file.has_value()) {
                return fail(AppConfigErrorCode::ManifestInvalid, "malformed node source");
            }
            node.source_file = std::move(*file);
            if (const JsonValue* line = source->find("line"); line != nullptr) {
                node.line = line->as_integer().value_or(0);
            }
            if (const JsonValue* column = source->find("column"); column != nullptr) {
                node.column = column->as_integer().value_or(0);
            }
        }
        manifest.nodes.push_back(std::move(node));
    }
    return manifest;
}

std::expected<AppManifest, AppConfigError> load_app_manifest(std::string_view config_path) {
    if (!harness::is_safe_artifact_path(config_path)) {
        return fail(AppConfigErrorCode::ConfigInvalid,
                    "app.config must be a safe relative path below the working directory");
    }
    const std::string config_file(config_path);
    bool too_large = false;
    const auto config_text = read_file(config_file, kMaxAppConfigBytes, too_large);
    if (too_large) {
        return fail(AppConfigErrorCode::ConfigTooLarge,
                    "'" + config_file + "' exceeds the size cap");
    }
    if (!config_text.has_value()) {
        return fail(AppConfigErrorCode::ConfigUnreadable, "cannot read '" + config_file + "'");
    }
    auto config = harness::parse_json(*config_text);
    if (!config.has_value() || !config->is_object()) {
        return fail(AppConfigErrorCode::ConfigInvalid,
                    "'" + config_file + "' is not a JSON object");
    }
    const JsonValue* schema = config->find("schemaVersion");
    if (schema == nullptr || schema->as_integer() != std::optional<std::int64_t>(1)) {
        return fail(AppConfigErrorCode::ConfigInvalid, "schemaVersion must be 1");
    }
    const JsonValue* app = config->find("app");
    if (app == nullptr || !app->is_object()) {
        return fail(AppConfigErrorCode::ConfigInvalid, "app must be an object");
    }
    std::string out_dir = "generated";
    if (const JsonValue* codegen = config->find("codegen"); codegen != nullptr) {
        const JsonValue* configured = codegen->find("outDir");
        if (!codegen->is_object() || (configured != nullptr && (!configured->is_string() ||
                                                                !harness::is_safe_artifact_path(
                                                                    *configured->as_string())))) {
            return fail(AppConfigErrorCode::ConfigInvalid,
                        "codegen.outDir must be a safe relative path");
        }
        if (configured != nullptr) {
            out_dir = std::string(*configured->as_string());
        }
    }

    const std::string manifest_file =
        join(join(parent_directory(config_file), out_dir), kManifestFileName);
    const auto manifest_text = read_file(manifest_file, kMaxManifestBytes, too_large);
    if (too_large) {
        return fail(AppConfigErrorCode::ManifestTooLarge,
                    "'" + manifest_file + "' exceeds the size cap");
    }
    if (!manifest_text.has_value()) {
        return fail(AppConfigErrorCode::ManifestMissing,
                    "cannot read '" + manifest_file + "'; run the code generator first");
    }
    return parse_manifest(*manifest_text);
}

} // namespace tessera::automation
