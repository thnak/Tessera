// SPDX-License-Identifier: MIT
//
// App config and automation manifest loading: the happy path against the checked-in fixture app,
// and deterministic errors for a missing, malformed, oversized, unsupported or ambiguous input.

#include "tessera/automation/app_config.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using tessera::automation::AppConfigErrorCode;

constexpr std::string_view kFixtureDirectory = TESSERA_HARNESS_FIXTURE_SCRIPT_DIR;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_config: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
}

std::string node(std::string_view id, std::string_view path, std::string_view kind = "View") {
    return R"({"componentId":"11","kind":")" + std::string(kind) + R"(","path":")" +
           std::string(path) + R"(","uiId":")" + std::string(id) + R"("})";
}

std::string manifest_with(const std::string& nodes, std::string_view version = "1") {
    return R"({"manifestVersion":)" + std::string(version) +
           R"(,"components":[{"componentId":"11","name":"A","source":"a.tsx"}],"nodes":[)" + nodes +
           "]}";
}

bool rejects(const std::string& text, AppConfigErrorCode code, const char* message) {
    const auto parsed = tessera::automation::parse_manifest(text);
    return expect(!parsed.has_value() && parsed.error().code == code, message);
}

} // namespace

int main() {
    using namespace tessera::automation;

    const std::string fixture_manifest =
        read_file(std::string(kFixtureDirectory) + "/app/generated/automation.manifest.json");
    const auto good = parse_manifest(fixture_manifest);
    if (expect(good.has_value(), "the fixture manifest parses")) {
        expect(good->project_salt == "factory", "salt is read");
        expect(good->components.size() == 1 && good->components[0].name == "Counter",
               "components are read");
        expect(good->nodes.size() == 4, "nodes are read in order");
        expect(good->nodes[2].ui_id == 7000000000000000003ULL && good->nodes[2].kind == "Button",
               "ids decode from decimal strings");
        expect(good->nodes[3].line == 8 && good->nodes[3].source_file == "ui/Counter.tsx",
               "source positions are read");
    }
    expect(parse_manifest(fixture_manifest).has_value() &&
               parse_manifest(fixture_manifest)->nodes.size() == good->nodes.size(),
           "parsing is repeatable");

    rejects("not json", AppConfigErrorCode::ManifestInvalid, "garbage is invalid");
    rejects("[]", AppConfigErrorCode::ManifestInvalid, "a non-object is invalid");
    rejects(manifest_with("", "2"), AppConfigErrorCode::ManifestUnsupportedVersion,
            "a future version is rejected");
    rejects(R"({"components":[],"nodes":[]})", AppConfigErrorCode::ManifestInvalid,
            "the version is required");
    rejects(manifest_with(node("0", "A/root[0]")), AppConfigErrorCode::ManifestInvalid,
            "a zero id is rejected");
    rejects(manifest_with(node("012", "A/root[0]")), AppConfigErrorCode::ManifestInvalid,
            "a leading zero is rejected");
    rejects(manifest_with(node("99999999999999999999", "A/root[0]")),
            AppConfigErrorCode::ManifestInvalid, "an id above uint64 is rejected");
    rejects(manifest_with(node("5", "A/root[0]") + "," + node("5", "A/root[0]/Text[0]")),
            AppConfigErrorCode::ManifestDuplicateId, "a duplicate id is rejected");
    rejects(manifest_with(node("5", "A/root[0]") + "," + node("6", "A/root[0]")),
            AppConfigErrorCode::ManifestInvalid, "a duplicate path is rejected");
    rejects(manifest_with(R"({"uiId":"5"})"), AppConfigErrorCode::ManifestInvalid,
            "a node missing fields is rejected");
    rejects(manifest_with(node("5", std::string(kMaxManifestStringBytes + 1, 'p'))),
            AppConfigErrorCode::ManifestInvalid, "an oversized path is rejected");
    rejects(std::string(kMaxManifestBytes + 1, ' '), AppConfigErrorCode::ManifestTooLarge,
            "an oversized document is rejected");

    std::string many;
    for (std::size_t i = 0; i <= kMaxManifestNodes; ++i) {
        many += (i == 0 ? "" : ",") + node(std::to_string(i + 1), "A/n[" + std::to_string(i) + "]");
    }
    rejects(manifest_with(many), AppConfigErrorCode::ManifestTooManyNodes,
            "too many nodes are rejected");

    // File-level loading, in a scratch tree below the working directory.
    const fs::path work = "app_config_scratch";
    std::error_code ignored;
    fs::remove_all(work, ignored);
    const auto load = [](const std::string& path) { return load_app_manifest(path); };

    write_file(work / "ok/tessera.json",
               read_file(std::string(kFixtureDirectory) + "/app/tessera.json"));
    write_file(work / "ok/generated/automation.manifest.json", fixture_manifest);
    const auto loaded = load("app_config_scratch/ok/tessera.json");
    expect(loaded.has_value() && loaded->nodes.size() == 4, "a project loads from tessera.json");

    write_file(work / "custom/tessera.json",
               R"({"schemaVersion":1,"app":{},"codegen":{"outDir":"gen"}})");
    write_file(work / "custom/gen/automation.manifest.json", fixture_manifest);
    expect(load("app_config_scratch/custom/tessera.json").has_value(),
           "codegen.outDir relocates the manifest");

    write_file(work / "nomanifest/tessera.json", R"({"schemaVersion":1,"app":{}})");
    const auto missing = load("app_config_scratch/nomanifest/tessera.json");
    expect(!missing.has_value() && missing.error().code == AppConfigErrorCode::ManifestMissing,
           "a missing manifest is ManifestMissing");

    const auto absent = load("app_config_scratch/nothing/tessera.json");
    expect(!absent.has_value() && absent.error().code == AppConfigErrorCode::ConfigUnreadable,
           "a missing config is ConfigUnreadable");

    write_file(work / "badjson/tessera.json", "{");
    expect(load("app_config_scratch/badjson/tessera.json").error().code ==
               AppConfigErrorCode::ConfigInvalid,
           "malformed config is ConfigInvalid");
    write_file(work / "badver/tessera.json", R"({"schemaVersion":2,"app":{}})");
    expect(load("app_config_scratch/badver/tessera.json").error().code ==
               AppConfigErrorCode::ConfigInvalid,
           "a wrong schemaVersion is ConfigInvalid");
    write_file(work / "noapp/tessera.json", R"({"schemaVersion":1})");
    expect(load("app_config_scratch/noapp/tessera.json").error().code ==
               AppConfigErrorCode::ConfigInvalid,
           "a missing app object is ConfigInvalid");
    write_file(work / "escape/tessera.json",
               R"({"schemaVersion":1,"app":{},"codegen":{"outDir":"../x"}})");
    expect(load("app_config_scratch/escape/tessera.json").error().code ==
               AppConfigErrorCode::ConfigInvalid,
           "an escaping outDir is ConfigInvalid");
    write_file(work / "big/tessera.json", R"({"schemaVersion":1,"app":{},"pad":")" +
                                              std::string(kMaxAppConfigBytes, 'x') + R"("})");
    expect(load("app_config_scratch/big/tessera.json").error().code ==
               AppConfigErrorCode::ConfigTooLarge,
           "an oversized config is ConfigTooLarge");
    write_file(work / "bigm/tessera.json", R"({"schemaVersion":1,"app":{}})");
    write_file(work / "bigm/generated/automation.manifest.json",
               std::string(kMaxManifestBytes + 1, ' '));
    expect(load("app_config_scratch/bigm/tessera.json").error().code ==
               AppConfigErrorCode::ManifestTooLarge,
           "an oversized manifest file is ManifestTooLarge");
    expect(load("/abs/tessera.json").error().code == AppConfigErrorCode::ConfigInvalid,
           "an absolute config path is rejected");
    expect(load("../tessera.json").error().code == AppConfigErrorCode::ConfigInvalid,
           "a parent-relative config path is rejected");

    fs::remove_all(work, ignored);
    return failures == 0 ? 0 : 1;
}
