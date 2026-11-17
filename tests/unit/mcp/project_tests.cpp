// SPDX-License-Identifier: MIT

// The project-folder tools of `tessera-mcp` (issue #406, docs/specs/designer-studio-v1.md section
// 8): `list_files`, the folder-wide `diagnostics` and `get_ir`, and the three ways the folder is
// chosen (an explicit `open_project`, the client's `roots`, the server's working directory). The
// listing and the compile run over real files under the build tree; the policy that judges them is
// the shared `tessera::schema` unit, whose own cases are in `schema_project_files_tests`.
//
// Usage: mcp_project_tests <path-to-tessera-mcp> <scratch-dir>
// Deterministic: fixed files, no clock, no network. Exit 0 pass, 1 failure, 2 usage.

#include "mcp_test_support.hpp"
#include "regenerate.hpp"
#include "session.hpp"
#include "tessera/schema/project_files.hpp"
#include "tessera/ui/automation/sha256.hpp"
#include "tool_session.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(TESSERA_NO_SUBPROCESS) ||                                                              \
    (defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#define TESSERA_MCP_NO_SUBPROCESS 1
#endif

namespace {

namespace fs = std::filesystem;
namespace mcp = tessera::mcp;
namespace harness = tessera::harness;
namespace cli = tessera::compiler::codegen::cli;
using mcp_test::call;
using mcp_test::decode;
using mcp_test::expect;
using mcp_test::result_is_error;
using mcp_test::result_text;
using mcp_test::string_at;

constexpr std::string_view kWho = "mcp_project";

void write_file(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The fixture project: a config, a broken entry (so the diagnostics document is not empty), a clean
// second component, a stylesheet, a C++ file and a dependency directory the walk must not enter.
fs::path make_project(const fs::path& base, std::string_view name, bool broken) {
    const fs::path root = base / name;
    fs::remove_all(root);
    write_file(root / "tessera.json",
               R"({"app":{"namespace":"mcp.fixture"},"entry":"ui/App.tsx"})");
    write_file(root / "ui/App.tsx", broken ? "export function App() {\n  return <NotAThing />;\n}\n"
                                           : "export function App() {\n  return <View />;\n}\n");
    write_file(root / "ui/Ok.tsx", "export function Ok() {\n  return <View />;\n}\n");
    write_file(root / "ui/app.css", ".a { color: red; }\n");
    write_file(root / "src/Impl.cpp", "int main() { return 0; }\n");
    write_file(root / "node_modules/dep/index.tsx", "export function Dep() { return <View />; }\n");
    return root;
}

[[maybe_unused]] std::string quoted_arg(const std::string& path) {
    return "\"" + path + "\"";
}

mcp::SessionReply send(mcp::McpSession& session, std::string_view line) {
    auto reply = session.handle_line(line);
    if (!reply) {
        std::cerr << kWho << ": the session could not handle a line\n";
        ++mcp_test::failures;
        return {};
    }
    return std::move(*reply);
}

harness::JsonValue listing_of(mcp::McpSession& session) {
    const harness::JsonValue response = call(session, "list_files", "{}", kWho);
    return decode(result_text(response), kWho);
}

const harness::JsonValue* file_entry(const harness::JsonValue& listing, std::string_view path) {
    const harness::JsonValue* files = listing.find("files");
    if (files == nullptr || files->as_array() == nullptr) {
        return nullptr;
    }
    for (const harness::JsonValue& file : *files->as_array()) {
        if (string_at(file, "path") == path) {
            return &file;
        }
    }
    return nullptr;
}

void open_project(mcp::McpSession& session, const fs::path& root) {
    const std::string args = R"({"path":")" + std::string([&] {
                                 std::string escaped;
                                 for (const char c : root.generic_string()) {
                                     if (c == '"' || c == '\\') {
                                         escaped.push_back('\\');
                                     }
                                     escaped.push_back(c);
                                 }
                                 return escaped;
                             }()) +
                             R"("})";
    const harness::JsonValue response = call(session, "open_project", args, kWho);
    expect(!result_is_error(response), "mcp_project: the fixture project opens");
}

// The file URI a client would put in `roots/list` for a directory.
std::string file_uri(const fs::path& directory) {
    const std::string path = fs::absolute(directory).generic_string();
    return path.front() == '/' ? "file://" + path : "file:///" + path;
}

void test_listing(const fs::path& base) {
    const fs::path root = make_project(base, "listing", true);
    mcp::McpSession session;
    mcp_test::greet(session, kWho);
    open_project(session, root);

    const harness::JsonValue listing = listing_of(session);
    expect(string_at(listing, "rootSource") == "argument", "mcp_project: an explicit root says so");
    expect(string_at(listing, "folder") == "listing", "mcp_project: the root is named by folder");

    struct Want {
        std::string_view path;
        std::string_view kind;
        bool readable;
    };
    const Want wanted[] = {
        {"tessera.json", "config", true}, {"ui/App.tsx", "source", true},
        {"ui/Ok.tsx", "source", true},    {"ui/app.css", "style", true},
        {"src/Impl.cpp", "other", false}, {"node_modules", "other", false},
    };
    for (const Want& want : wanted) {
        const harness::JsonValue* entry = file_entry(listing, want.path);
        expect(entry != nullptr, "mcp_project: the listing names " + std::string(want.path));
        if (entry == nullptr) {
            continue;
        }
        expect(string_at(*entry, "kind") == want.kind,
               "mcp_project: " + std::string(want.path) + " has kind " + std::string(want.kind));
        const harness::JsonValue* readable = entry->find("readable");
        expect(readable != nullptr && readable->as_bool() == std::optional<bool>(want.readable),
               "mcp_project: " + std::string(want.path) + " readability");
        expect((entry->find("digest") != nullptr) == want.readable,
               "mcp_project: only a readable file carries a digest");
    }
    expect(file_entry(listing, "node_modules/dep/index.tsx") == nullptr,
           "mcp_project: a pruned directory is one entry and is never entered");

    const harness::JsonValue* app = file_entry(listing, "ui/App.tsx");
    expect(app != nullptr && string_at(*app, "digest") == tessera::schema::project_file_digest(
                                                              read_file(root / "ui/App.tsx")),
           "mcp_project: the digest is the shared digest of the file bytes");

    // Absolute host paths never leave the server.
    const std::string text = result_text(call(session, "list_files", "{}", kWho));
    expect(text.find(root.generic_string()) == std::string::npos &&
               text.find(root.parent_path().generic_string()) == std::string::npos,
           "mcp_project: the listing holds no absolute path");

    // Sorted by path, in byte order: the same listing every time.
    const harness::JsonValue::Array* files = listing.find("files")->as_array();
    bool sorted = true;
    for (std::size_t i = 1; i < files->size(); ++i) {
        sorted = sorted && string_at((*files)[i - 1], "path") < string_at((*files)[i], "path");
    }
    expect(sorted, "mcp_project: the listing is sorted by path");
}

// The folder-wide document equals what `tessera_codegen --diagnostics-json` writes for the same
// files, run from the project folder with the same relative inputs.
void test_folder_diagnostics_parity(const fs::path& base) {
    const fs::path root = make_project(base, "parity", true);
    mcp::McpSession session;
    mcp_test::greet(session, kWho);
    open_project(session, root);

    const std::string tool_document = result_text(call(session, "diagnostics", "{}", kWho));
    expect(!tool_document.empty(), "mcp_project: the folder diagnostics call answers");

    const fs::path previous = fs::current_path();
    fs::current_path(root);
    cli::Options options;
    options.inputs = {"ui/App.tsx", "ui/Ok.tsx"}; // the entry first, then the rest by path
    options.diagnostics_json_path = (base / "parity.diagnostics.json").string();
    const cli::Outcome outcome = cli::run(options);
    fs::current_path(previous);
    expect(outcome.exit_code == 1, "mcp_project: the CLI finds the broken entry");
    expect(tool_document == read_file(options.diagnostics_json_path),
           "mcp_project: the folder document is byte-identical to the CLI's");

    const harness::JsonValue parsed = decode(tool_document, kWho);
    const harness::JsonValue* list = parsed.find("diagnostics");
    expect(list != nullptr && list->as_array() != nullptr && list->as_array()->size() == 1,
           "mcp_project: one diagnostic from the broken entry");
    expect(string_at(parsed, "file") == "ui/App.tsx",
           "mcp_project: spans are project-relative, never absolute");

    const harness::JsonValue ir = call(session, "get_ir", "{}", kWho);
    expect(result_is_error(ir) && result_text(ir) == tool_document,
           "mcp_project: get_ir over a broken folder returns the diagnostics with isError");
}

void test_folder_ir(const fs::path& base) {
    const fs::path root = make_project(base, "clean", false);
    mcp::McpSession session;
    mcp_test::greet(session, kWho);
    open_project(session, root);

    const harness::JsonValue response = call(session, "get_ir", "{}", kWho);
    expect(!result_is_error(response), "mcp_project: a clean folder lowers");
    const harness::JsonValue module = decode(result_text(response), kWho);
    expect(module.find("components") != nullptr && module.find("diagnosticsVersion") == nullptr,
           "mcp_project: get_ir returns the IR document");
    const harness::JsonValue* components = module.find("components");
    expect(components != nullptr && components->as_array() != nullptr &&
               components->as_array()->size() == 2,
           "mcp_project: both source files of the folder are in the module");

    const harness::JsonValue diagnostics = call(session, "diagnostics", "{}", kWho);
    const harness::JsonValue document = decode(result_text(diagnostics), kWho);
    const harness::JsonValue* list = document.find("diagnostics");
    expect(list != nullptr && list->as_array() != nullptr && list->as_array()->empty(),
           "mcp_project: a clean folder reports no diagnostics");

    // The in-memory mode is untouched: `source` still compiles the text it carries.
    const harness::JsonValue inline_call = call(
        session, "diagnostics",
        R"({"source":"export function Tag() {\n  return <NotAThing />;\n}\n","file":"ui/Tag.tsx"})",
        kWho);
    expect(string_at(decode(result_text(inline_call), kWho), "file") == "ui/Tag.tsx",
           "mcp_project: a call with `source` keeps the in-memory behaviour");
}

void test_root_resolution(const fs::path& base) {
    const fs::path first = make_project(base, "root_a", false);
    const fs::path second = make_project(base, "root_b", false);

    // The client declares `roots`: after `initialized` the server asks, and the answer is the root.
    {
        mcp::McpSession session;
        const auto init = send(
            session,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{"roots":{"listChanged":true}},"clientInfo":{"name":"t","version":"1"}}})");
        expect(init.answered, "mcp_project: the handshake with a roots capability is answered");
        const auto acknowledged =
            send(session, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        expect(!acknowledged.answered, "mcp_project: the acknowledgement is not answered");
        expect(acknowledged.outbound.size() == 1,
               "mcp_project: the server asks for the roots after the handshake");
        if (acknowledged.outbound.size() == 1) {
            const harness::JsonValue request = decode(acknowledged.outbound.front(), kWho);
            expect(string_at(request, "method") == "roots/list",
                   "mcp_project: the server's request is roots/list");
            expect(string_at(request, "id") == "tessera-roots-1",
                   "mcp_project: the request id is the documented one");
        }
        // An answer with another id is not ours and changes nothing.
        send(session, R"({"jsonrpc":"2.0","id":"someone-else","result":{"roots":[{"uri":")" +
                          file_uri(second) + R"("}]}})");
        expect(!session.state().project().has_value(),
               "mcp_project: a response with an unknown id is dropped");

        send(session, R"({"jsonrpc":"2.0","id":"tessera-roots-1","result":{"roots":[{"uri":")" +
                          file_uri(first) + R"("}]}})");
        const harness::JsonValue listing = listing_of(session);
        expect(string_at(listing, "rootSource") == "roots",
               "mcp_project: the client's root is used");
        expect(string_at(listing, "folder") == "root_a", "mcp_project: the first root is chosen");

        // The client's roots changed: ask again, and the new answer wins.
        const auto changed =
            send(session, R"({"jsonrpc":"2.0","method":"notifications/roots/list_changed"})");
        expect(changed.outbound.size() == 1, "mcp_project: a changed root list is asked for again");
        send(session, R"({"jsonrpc":"2.0","id":"tessera-roots-2","result":{"roots":[{"uri":")" +
                          file_uri(second) + R"("}]}})");
        expect(string_at(listing_of(session), "folder") == "root_b",
               "mcp_project: the re-resolved root replaces the old one");

        // An explicit open_project overrides both, and a later roots answer cannot undo it.
        open_project(session, first);
        send(session, R"({"jsonrpc":"2.0","method":"notifications/roots/list_changed"})");
        send(session, R"({"jsonrpc":"2.0","id":"tessera-roots-3","result":{"roots":[{"uri":")" +
                          file_uri(second) + R"("}]}})");
        const harness::JsonValue pinned = listing_of(session);
        expect(string_at(pinned, "rootSource") == "argument" &&
                   string_at(pinned, "folder") == "root_a",
               "mcp_project: an explicit open_project overrides the client's roots");
    }

    // Without the capability the server never asks.
    {
        mcp::McpSession session;
        mcp_test::greet(session, kWho);
        const auto acknowledged =
            send(session, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        expect(acknowledged.outbound.empty(),
               "mcp_project: a client without the roots capability is not asked");
    }

    // The working directory is the root when nothing else says; a remote root is refused.
    {
        const fs::path previous = fs::current_path();
        fs::current_path(first);
        mcp::McpSession session;
        mcp_test::greet(session, kWho);
        const harness::JsonValue listing = listing_of(session);
        expect(string_at(listing, "rootSource") == "cwd" &&
                   string_at(listing, "folder") == "root_a",
               "mcp_project: the working directory is the default root");
        expect(file_entry(listing, "ui/App.tsx") != nullptr,
               "mcp_project: and its files are listed with no open_project");
        fs::current_path(previous);
    }
    {
        const fs::path previous = fs::current_path();
        fs::current_path(second);
        mcp::McpSession session;
        send(
            session,
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{"roots":{}},"clientInfo":{"name":"t","version":"1"}}})");
        send(session, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        send(
            session,
            R"({"jsonrpc":"2.0","id":"tessera-roots-1","result":{"roots":[{"uri":"file://other-host/share/p"},{"uri":"https://example.test/x"}]}})");
        expect(string_at(listing_of(session), "rootSource") == "cwd",
               "mcp_project: a remote or non-file root falls back to the working directory");
        fs::current_path(previous);
    }

    expect(mcp::path_from_file_uri("file:///home/dev/my%20project") == "/home/dev/my project",
           "mcp_project: a POSIX file URI decodes");
    expect(mcp::path_from_file_uri("file:///C:/Users/dev/p") == "C:/Users/dev/p",
           "mcp_project: a Windows drive URI drops the leading slash");
    expect(mcp::path_from_file_uri("file://localhost/home/dev") == "/home/dev",
           "mcp_project: localhost names this machine");
    expect(mcp::path_from_file_uri("file://host/home/dev").empty(),
           "mcp_project: another host is refused");
    expect(mcp::path_from_file_uri("http://example.test/").empty(),
           "mcp_project: another scheme is refused");
    expect(mcp::path_from_file_uri("file:///a%00b").empty(),
           "mcp_project: an encoded NUL is refused");
    expect(mcp::path_from_file_uri("file:///a%2").empty(),
           "mcp_project: a truncated escape is refused");
}

void test_refusals(const fs::path& base) {
    // A symbolic link that leaves the root is refused with the named error. Creating one may need a
    // privilege (Windows), so the case is skipped, loudly, when the host cannot.
    {
        const fs::path root = make_project(base, "escape", false);
        const fs::path outside = make_project(base, "escape_target", false);
        std::error_code error;
        fs::create_directory_symlink(outside, root / "link", error);
        if (error) {
            std::cerr << "mcp_project: symbolic links unavailable (" << error.message()
                      << "); the escape case is skipped\n";
        } else {
            mcp::McpSession session;
            mcp_test::greet(session, kWho);
            open_project(session, root);
            const harness::JsonValue response = call(session, "list_files", "{}", kWho);
            expect(result_is_error(response) &&
                       result_text(response).starts_with("TESSERA_SEC_PATH_ESCAPE:"),
                   "mcp_project: a symlink out of the root is a named refusal");
            expect(result_text(response).find("link") != std::string::npos &&
                       result_text(response).find(outside.generic_string()) == std::string::npos,
                   "mcp_project: it names the link, never an absolute path");
            const harness::JsonValue diagnostics = call(session, "diagnostics", "{}", kWho);
            expect(result_is_error(diagnostics),
                   "mcp_project: the compile tools refuse the same folder");
        }
    }

    // Too many files: the count limit names itself.
    {
        const fs::path root = make_project(base, "many", false);
        for (std::size_t i = 0; i < tessera::schema::kMaxProjectFiles; ++i) {
            write_file(root / "ui" / ("n" + std::to_string(i) + ".txt"), "x");
        }
        mcp::McpSession session;
        mcp_test::greet(session, kWho);
        open_project(session, root);
        const harness::JsonValue response = call(session, "list_files", "{}", kWho);
        expect(result_is_error(response) &&
                   result_text(response).starts_with("TESSERA_SEC_FILE_LIMIT:"),
               "mcp_project: a folder over the file limit is refused with the named error");
    }

    // One accepted file over 1 MiB: refused, naming the file.
    {
        const fs::path root = make_project(base, "big", false);
        write_file(root / "ui/Big.tsx",
                   std::string(tessera::schema::kMaxProjectFileBytes + 1, ' '));
        mcp::McpSession session;
        mcp_test::greet(session, kWho);
        open_project(session, root);
        const harness::JsonValue response = call(session, "list_files", "{}", kWho);
        expect(result_is_error(response) &&
                   result_text(response).starts_with("TESSERA_SEC_FILE_LIMIT:") &&
                   result_text(response).find("ui/Big.tsx") != std::string::npos,
               "mcp_project: an over-size source file names itself");
    }

    // The same size in a file that is not accepted costs nothing: it is listed and never read.
    {
        const fs::path root = make_project(base, "big_other", false);
        write_file(root / "src/blob.bin",
                   std::string(tessera::schema::kMaxProjectFileBytes + 1, 'x'));
        mcp::McpSession session;
        mcp_test::greet(session, kWho);
        open_project(session, root);
        const harness::JsonValue listing = listing_of(session);
        const harness::JsonValue* blob = file_entry(listing, "src/blob.bin");
        expect(blob != nullptr && string_at(*blob, "kind") == "other",
               "mcp_project: an unaccepted large file is listed as other");
    }
}

// ---- `generate` (issue #407) ----------------------------------------------------------------

std::string sha256_of(std::string_view bytes) {
    return "sha256:" + tessera::ui::automation::to_hex(tessera::ui::automation::sha256(bytes));
}

bool exists_dir(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}

harness::JsonValue generate_call(mcp::McpSession& session, std::string_view arguments) {
    const harness::JsonValue response = call(session, "generate", arguments, kWho);
    return response;
}

std::vector<std::string> strings_at(const harness::JsonValue& object, std::string_view key) {
    std::vector<std::string> out;
    const harness::JsonValue* list = object.find(key);
    if (list != nullptr && list->as_array() != nullptr) {
        for (const harness::JsonValue& item : *list->as_array()) {
            if (item.is_string()) {
                out.emplace_back(*item.as_string());
            }
        }
    }
    return out;
}

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

#if !defined(TESSERA_MCP_NO_SUBPROCESS)
// The real process started in the project folder, with no `open_project` and no roots capability:
// `list_files` answers for the working directory.
void test_process_in_project_directory(const fs::path& executable, const fs::path& base) {
    const fs::path root = make_project(base, "process_cwd", false);
    const std::string transcript =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}}})"
        "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_files","arguments":{}}})"
        "\n";
    const fs::path input = base / "process_cwd.in.jsonl";
    const fs::path output = base / "process_cwd.out.jsonl";
    write_file(input, transcript);
#ifdef _WIN32
    const std::string command =
        "cd /d " + quoted_arg(root.string()) + " && call " + quoted_arg(executable.string()) +
        " < " + quoted_arg(input.string()) + " > " + quoted_arg(output.string()) + " 2>&1";
#else
    const std::string command = "cd '" + root.string() + "' && '" + executable.string() + "' < '" +
                                input.string() + "' > '" + output.string() + "' 2>&1";
#endif
    (void)std::system(command.c_str());
    const std::string responses = read_file(output);
    std::vector<std::string> lines;
    std::istringstream stream(responses);
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    expect(lines.size() == 2, "mcp_project: the server answers the two requests and nothing else");
    if (lines.size() == 2) {
        const harness::JsonValue reply = decode(lines[1], kWho);
        const harness::JsonValue* result = reply.find("result");
        expect(result != nullptr && !result_is_error(reply), "mcp_project: list_files succeeds");
        const harness::JsonValue listing = decode(result_text(reply), kWho);
        expect(string_at(listing, "rootSource") == "cwd" &&
                   string_at(listing, "folder") == "process_cwd",
               "mcp_project: a server started in a folder lists that folder");
        expect(file_entry(listing, "ui/App.tsx") != nullptr,
               "mcp_project: with no open_project, over a real pipe");
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <tessera-mcp> <scratch-dir>\n";
        return 2;
    }
    const fs::path executable(argv[1]);
    const fs::path base = fs::absolute(fs::path(argv[2]));
    std::error_code ignored;
    fs::remove_all(base, ignored);
    fs::create_directories(base);

    test_listing(base);
    test_folder_diagnostics_parity(base);
    test_folder_ir(base);
    test_root_resolution(base);
    test_refusals(base);
    test_generate(base);
    test_generate_refusals(base);
#if !defined(TESSERA_MCP_NO_SUBPROCESS)
    if (fs::is_regular_file(executable, ignored)) {
        test_process_in_project_directory(executable, base);
    } else {
        std::cerr << "mcp_project: no server at " << executable << "\n";
        ++mcp_test::failures;
    }
#endif
    return mcp_test::failures == 0 ? 0 : 1;
}
