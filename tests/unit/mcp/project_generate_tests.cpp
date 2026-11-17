// SPDX-License-Identifier: MIT

// The `generate` tool of `tessera-mcp` (issue #407, docs/specs/designer-studio-v1.md section 8):
// the dry-run default, the byte-identity claim against `tessera_codegen --write`, the developer
// classes that are scaffolded once and never edited, and the refusals that must write nothing. The
// cases share the fixture project and the JSON-RPC helpers of project_fixtures.hpp; the
// project-folder cases of issue #406 are in project_tests.cpp, which also owns main().

#include "project_fixtures.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace mcp_project {

namespace {

void test_generate(const fs::path& base) {
    const fs::path root = make_project(base, "gen", false);
    mcp::McpSession session;
    mcp_test::greet(session, kWho);
    open_project(session, root);

    // The default is a dry run: it reports, and the disk does not change.
    const harness::JsonValue dry_response = generate_call(session, "{}");
    expect(!result_is_error(dry_response), "mcp_project: a clean project generates");
    const harness::JsonValue dry = decode(result_text(dry_response), kWho);
    const harness::JsonValue* dry_write = dry.find("write");
    expect(dry_write != nullptr && dry_write->as_bool() == std::optional<bool>(false),
           "mcp_project: generate defaults to a dry run");
    expect(string_at(dry, "outDir") == "generated",
           "mcp_project: the output directory defaults to `generated`");
    const harness::JsonValue* dry_drift = dry.find("drift");
    expect(dry_drift != nullptr && dry_drift->as_bool() == std::optional<bool>(true),
           "mcp_project: a never-generated project is drift");
    expect(dry.find("exitCode") != nullptr && dry.find("exitCode")->as_integer() == 1,
           "mcp_project: a dry run with drift has the CLI's --check exit code");
    expect(!exists_dir(root / "generated"), "mcp_project: a dry run creates no directory");
    const harness::JsonValue* dry_files = dry.find("files");
    expect(dry_files != nullptr && dry_files->as_array() != nullptr &&
               !dry_files->as_array()->empty(),
           "mcp_project: the summary lists the generated files");
    std::vector<std::string> paths;
    std::vector<std::string> digests; // parallel to `paths`
    if (dry_files != nullptr && dry_files->as_array() != nullptr) {
        for (const harness::JsonValue& file : *dry_files->as_array()) {
            paths.push_back(string_at(file, "path"));
            expect(string_at(file, "status") == "new", "mcp_project: a first run is all `new`");
            const std::string digest = string_at(file, "digest");
            digests.push_back(digest);
            expect(digest.size() == 7 + 64 && digest.starts_with("sha256:"),
                   "mcp_project: the digest is a SHA-256");
            expect(paths.back().starts_with("generated/"),
                   "mcp_project: generated paths are under the output directory");
        }
    }
    expect(!strings_at(dry, "scaffold").empty(),
           "mcp_project: the dry run names the developer classes it would scaffold");
    expect(strings_at(dry, "written").empty(), "mcp_project: a dry run writes nothing");

    // `path` returns one generated file's text, and always only reads.
    if (!paths.empty()) {
        const harness::JsonValue one =
            generate_call(session, R"({"write":true,"path":")" + paths.front() + R"("})");
        expect(!result_is_error(one) && !result_text(one).empty(),
               "mcp_project: `path` returns the file's text");
        expect(!exists_dir(root / "generated"),
               "mcp_project: reading one file never writes, even with write:true");
        const harness::JsonValue by_name = generate_call(
            session,
            R"({"path":")" + paths.front().substr(std::string("generated/").size()) + R"("})");
        expect(result_text(by_name) == result_text(one),
               "mcp_project: a bare name finds the same file");
        const harness::JsonValue unknown =
            generate_call(session, R"({"path":"generated/nope.hpp"})");
        expect(result_is_error(unknown) &&
                   result_text(unknown).starts_with("TESSERA_MCP_INVALID_PARAMS:"),
               "mcp_project: an unknown generated file is a named error");
        expect(string_at(decode(result_text(dry_response), kWho), "outDir") == "generated" &&
                   result_text(unknown).find(root.generic_string()) == std::string::npos,
               "mcp_project: no absolute path in the error");
    }

    // The files `write: true` produces are byte-identical to `tessera_codegen --write` for the same
    // inputs, run from the project folder.
    const fs::path cli_out = base / "gen_cli_out";
    {
        const fs::path previous = fs::current_path();
        fs::current_path(root);
        cli::Options options;
        options.inputs = {"ui/App.tsx", "ui/Ok.tsx"};
        options.output_dir = cli_out.string();
        const cli::Outcome outcome = cli::run(options);
        fs::current_path(previous);
        expect(outcome.exit_code == 0, "mcp_project: the CLI generates the same project");
    }
    const harness::JsonValue written_response = generate_call(session, R"({"write":true})");
    expect(!result_is_error(written_response), "mcp_project: generate with write succeeds");
    const harness::JsonValue written = decode(result_text(written_response), kWho);
    const harness::JsonValue* write_flag = written.find("write");
    expect(write_flag != nullptr && write_flag->as_bool() == std::optional<bool>(true),
           "mcp_project: the result says it wrote");
    expect(strings_at(written, "written").size() == paths.size(),
           "mcp_project: every new file is reported as written");
    for (const std::string& relative : paths) {
        const std::string name = relative.substr(std::string("generated/").size());
        const std::string ours = read_file(root / relative);
        expect(!ours.empty(), "mcp_project: " + relative + " is on disk after write");
        expect(ours == read_file(cli_out / name),
               "mcp_project: " + relative + " is byte-identical to the CLI's output");
        const std::size_t index = static_cast<std::size_t>(&relative - paths.data());
        expect(index < digests.size() && digests[index] == sha256_of(read_file(cli_out / name)),
               "mcp_project: " + relative + " digest is the SHA-256 of the CLI's bytes");
    }

    // A second call reports no drift, and a second write writes nothing.
    const harness::JsonValue again = decode(result_text(generate_call(session, "{}")), kWho);
    const harness::JsonValue* again_drift = again.find("drift");
    expect(again_drift != nullptr && again_drift->as_bool() == std::optional<bool>(false) &&
               again.find("exitCode")->as_integer() == 0,
           "mcp_project: after a write the project is in step");
    const harness::JsonValue* again_files = again.find("files");
    bool all_unchanged = again_files != nullptr && again_files->as_array() != nullptr;
    if (all_unchanged) {
        for (const harness::JsonValue& file : *again_files->as_array()) {
            all_unchanged = all_unchanged && string_at(file, "status") == "unchanged";
        }
    }
    expect(all_unchanged, "mcp_project: every file is unchanged on the second call");
    const harness::JsonValue rewrite =
        decode(result_text(generate_call(session, R"({"write":true})")), kWho);
    expect(strings_at(rewrite, "written").empty(), "mcp_project: a second write writes nothing");

    // An existing developer class is never modified, and a drifted generated base is restored.
    const std::vector<std::string> scaffold = strings_at(written, "scaffold");
    expect(!scaffold.empty(), "mcp_project: the write scaffolded developer classes");
    std::vector<std::pair<fs::path, std::string>> developer_before;
    for (const std::string& relative : scaffold) {
        expect(fs::is_regular_file(root / relative),
               "mcp_project: " + relative + " was scaffolded on disk");
        std::ofstream out(root / relative, std::ios::binary | std::ios::app);
        out << "\n// my own code\n";
        out.close();
        developer_before.emplace_back(root / relative, read_file(root / relative));
    }
    if (!paths.empty()) {
        std::ofstream out(root / paths.front(), std::ios::binary | std::ios::app);
        out << "// tampered\n";
    }
    const harness::JsonValue drifted = decode(result_text(generate_call(session, "{}")), kWho);
    expect(drifted.find("drift") != nullptr &&
               drifted.find("drift")->as_bool() == std::optional<bool>(true),
           "mcp_project: a drifted generated file is reported");
    const harness::JsonValue* drifted_files = drifted.find("files");
    expect(drifted_files != nullptr && drifted_files->as_array() != nullptr && !paths.empty() &&
               string_at(drifted_files->as_array()->front(), "status") == "changed",
           "mcp_project: it is `changed`");
    (void)generate_call(session, R"({"write":true})");
    expect(!paths.empty() &&
               read_file(root / paths.front()) == read_file(cli_out / paths.front().substr(10)),
           "mcp_project: a write restores the generated base");
    for (const auto& [file, before] : developer_before) {
        expect(read_file(file) == before, "mcp_project: a developer class is untouched by a write");
    }

    // Wrong argument types are refused before anything is generated.
    expect(mcp_test::error_code(generate_call(session, R"({"write":"yes"})")) != 0,
           "mcp_project: a non-boolean `write` is refused");
    expect(mcp_test::error_code(generate_call(session, R"({"path":7})")) != 0,
           "mcp_project: a non-string `path` is refused");
}

void test_generate_refusals(const fs::path& base) {
    // A blocking diagnostic writes nothing and returns the document.
    {
        const fs::path root = make_project(base, "gen_broken", true);
        mcp::McpSession session;
        mcp_test::greet(session, kWho);
        open_project(session, root);
        const harness::JsonValue response = generate_call(session, R"({"write":true})");
        expect(result_is_error(response), "mcp_project: a broken project does not generate");
        expect(decode(result_text(response), kWho).find("diagnostics") != nullptr,
               "mcp_project: it returns the diagnostics document");
        expect(!exists_dir(root / "generated"), "mcp_project: nothing is written on a diagnostic");
    }
    // `codegen.outDir` from tessera.json is honoured; one that leaves the root is refused.
    {
        const fs::path root = make_project(base, "gen_outdir", false);
        write_file(
            root / "tessera.json",
            R"({"app":{"namespace":"mcp.fixture"},"entry":"ui/App.tsx","codegen":{"outDir":"out/gen"}})");
        mcp::McpSession session;
        mcp_test::greet(session, kWho);
        open_project(session, root);
        const harness::JsonValue summary =
            decode(result_text(generate_call(session, R"({"write":true})")), kWho);
        expect(string_at(summary, "outDir") == "out/gen",
               "mcp_project: outDir comes from the config");
        expect(fs::is_directory(root / "out/gen"),
               "mcp_project: files go to the configured directory");
    }
    for (const char* bad :
         {"../zz_outside", "/tmp/zz_outside", "a/../../zz_outside", "node_modules/gen"}) {
        const fs::path root = make_project(base, "gen_badout", false);
        write_file(root / "tessera.json",
                   std::string(R"({"app":{"namespace":"mcp.fixture"},"codegen":{"outDir":")") +
                       bad + R"("}})");
        mcp::McpSession session;
        mcp_test::greet(session, kWho);
        open_project(session, root);
        const harness::JsonValue response = generate_call(session, R"({"write":true})");
        expect(result_is_error(response) &&
                   result_text(response).starts_with("TESSERA_SEC_PATH_ESCAPE:"),
               std::string("mcp_project: outDir `") + bad + "` is refused");
        expect(!fs::exists(base / "zz_outside") && !fs::exists("/tmp/zz_outside"),
               "mcp_project: nothing was written outside the root");
    }
    // The output directory as a symbolic link, even one that stays inside the root, is refused.
    {
        const fs::path root = make_project(base, "gen_link", false);
        const fs::path elsewhere = root / "elsewhere";
        fs::create_directories(elsewhere);
        std::error_code error;
        fs::create_directory_symlink(elsewhere, root / "generated", error);
        if (error) {
            std::cerr
                << "mcp_project: symbolic links unavailable; the output-link case is skipped\n";
        } else {
            mcp::McpSession session;
            mcp_test::greet(session, kWho);
            open_project(session, root);
            const harness::JsonValue response = generate_call(session, R"({"write":true})");
            expect(result_is_error(response) &&
                       result_text(response).starts_with("TESSERA_SEC_PATH_ESCAPE:"),
                   "mcp_project: a linked output directory is refused");
            expect(fs::is_empty(elsewhere), "mcp_project: nothing was written through the link");
        }
    }
}

} // namespace

void project_generate(const fs::path& base) {
    test_generate(base);
}

void project_generate_refusals(const fs::path& base) {
    test_generate_refusals(base);
}

} // namespace mcp_project
