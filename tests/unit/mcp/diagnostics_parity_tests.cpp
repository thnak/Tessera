// SPDX-License-Identifier: MIT

// The issue's acceptance for #172: "a scripted MCP client compiles a sample and receives
// diagnostics identical to the CLI harness".
//
// The MCP `diagnostics` tool and `tessera_codegen --diagnostics-json` must produce the same
// document for the same input. The tool reaches that by calling the same `diagnose_source()` the
// preview page uses, and that function is already proven against the file-driven CLI driver in
// `codegen_diagnostics_json_tests.cpp`; this test proves the *adapter* did not add a stage, a
// message, or a truncation on the way -- which is what a client actually gets. It runs both sides
// over one corpus in process (no subprocess, no %TEMP%), so it works in a confined sandbox.
//
// A corpus that only holds valid files would pass by comparing two empty documents, so every case
// declares the minimum number of diagnostics it must report and the comparison is byte-exact.

#include "regenerate.hpp"
#include "session.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_write.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace fs = std::filesystem;
namespace cli = tessera::compiler::codegen::cli;
namespace harness = tessera::harness;
namespace mcp = tessera::mcp;

int failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "mcp_diagnostics_parity: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_file(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// The text content block of a `tools/call` response, which is where a tool puts its document.
std::string tool_text(const harness::JsonValue& response) {
    const harness::JsonValue* result = response.find("result");
    const harness::JsonValue* content = result == nullptr ? nullptr : result->find("content");
    const harness::JsonValue::Array* items = content == nullptr ? nullptr : content->as_array();
    if (items == nullptr || items->empty()) {
        return {};
    }
    const harness::JsonValue* text = items->front().find("text");
    if (text == nullptr || !text->is_string()) {
        return {};
    }
    return std::string(*text->as_string());
}

// One `tools/call` line through the session, decoded. Every case goes through the real dispatch,
// not through the tool body directly, so the argument handling is covered too.
std::string call_diagnostics(mcp::McpSession& session, std::string_view source,
                             std::string_view file, std::uint64_t id) {
    // The source is embedded with the harness writer so a newline or quote in the corpus cannot
    // break the request line.
    harness::JsonValue arguments = harness::JsonValue::object();
    arguments.set("source", harness::JsonValue::string(std::string(source)));
    arguments.set("file", harness::JsonValue::string(std::string(file)));
    harness::JsonValue params = harness::JsonValue::object();
    params.set("name", harness::JsonValue::string("diagnostics"));
    params.set("arguments", std::move(arguments));
    harness::JsonValue request = harness::JsonValue::object();
    request.set("jsonrpc", harness::JsonValue::string("2.0"));
    request.set("id", harness::JsonValue::integer(static_cast<std::int64_t>(id)));
    request.set("method", harness::JsonValue::string("tools/call"));
    request.set("params", std::move(params));

    auto line = tessera::harness::to_json(request);
    if (!line) {
        expect(false, "the request line encodes");
        return {};
    }
    auto reply = session.handle_line(*line);
    if (!reply) {
        expect(false, "the session answers a tools/call");
        return {};
    }
    auto parsed = harness::parse_json(reply->reply);
    if (!parsed) {
        expect(false, "the response is strict JSON");
        return {};
    }
    return tool_text(*parsed);
}

std::size_t diagnostic_count(std::string_view document) {
    auto parsed = harness::parse_json(document);
    if (!parsed) {
        return 0;
    }
    const harness::JsonValue* list = parsed->find("diagnostics");
    if (list == nullptr || list->as_array() == nullptr) {
        return 0;
    }
    return list->as_array()->size();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: mcp_diagnostics_parity_tests <scratch-dir>\n";
        return 2;
    }
    const fs::path work(argv[1]);
    std::error_code ignored;
    fs::remove_all(work, ignored);
    fs::create_directories(work);
    // Absolute, because each case below changes the process's working directory to the case root:
    // a relative scratch path would then resolve inside the tree being compiled, and the document
    // would be written where the case cannot read it back.
    const fs::path scratch = fs::absolute(work);

    struct Case {
        const char* name;
        const char* file; // project-relative, POSIX: both sides must see the same string
        const char* source;
        int minimum; // anti-vacuity: a diagnosing case must diagnose on both sides
    };
    // The same corpus `codegen_diagnostics_json_tests.cpp` and
    // scripts/ci/wasm_diagnostics_parity.py use, so the three consumers of one document agree about
    // what "the same input" means.
    const Case cases[] = {
        {"clean", "ui/Clear.tsx", "export function Clear() {\n  return <View />;\n}\n", 0},
        {"jsx-lex", "ui/Lex.tsx", "export function Lex() {\n  return <View>}</View>;\n}\n", 1},
        {"jsx-parse", "ui/Parse.tsx", "export function Parse() {\n  return <View></Text>;\n}\n", 1},
        {"lowering", "ui/Tag.tsx", "export function Tag() {\n  return <NotAThing />;\n}\n", 1},
        {"react-rules", "ui/Hook.tsx",
         "export function Hook() {\n  const v = useAnimatedValue(0, 12);\n  return <View "
         "/>;\n}\n",
         1},
        {"span-not-first-line", "ui/Deep.tsx",
         "// a leading comment\n// and a second one\nexport function Deep() {\n  return <Nope "
         "/>;\n}\n",
         1},
        {"utf8-offset", "ui/Utf8.tsx",
         // A multi-byte line before the error, so the span's byte offset and its code-point column
         // must both survive the adapter unchanged.
         "// caf\xC3\xA9 na\xC3\xAFve \xC3\xBC"
         "ber\nexport function Utf8() {\n  return <Nope />;\n}\n",
         1},
    };

    mcp::McpSession session;
    auto handshake = session.handle_line(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"mcp_diagnostics_parity","version":"1"}}})");
    expect(handshake.has_value() && handshake->answered, "the session accepts the handshake");

    std::uint64_t next_id = 2;
    for (const Case& test_case : cases) {
        // The CLI side runs with its working directory at the case root and a relative input path,
        // so every span names the same project-relative string the MCP side is given.
        const fs::path case_dir = scratch / test_case.name;
        write_file(case_dir / test_case.file, test_case.source);
        const fs::path document = scratch / (std::string(test_case.name) + ".cli.json");

        cli::Options options;
        options.inputs.emplace_back(test_case.file);
        options.diagnostics_json_path = document.string();
        const fs::path previous = fs::current_path();
        fs::current_path(case_dir);
        const cli::Outcome outcome = cli::run(options);
        fs::current_path(previous);
        expect(outcome.exit_code <= 1,
               std::string("the CLI run of `") + test_case.name + "` is not a setup failure");

        const std::string from_cli = read_file(document);
        const std::string from_mcp =
            call_diagnostics(session, test_case.source, test_case.file, next_id++);

        // Byte-exact, including the `file` field: both sides are given the same project-relative
        // string, so a document that differs at all is a real drift in which stage stops first, in
        // a message, or in a span (docs/specs/compiler-diagnostics-v1.md section 5).
        expect(from_cli == from_mcp,
               std::string("the MCP diagnostics document equals the CLI harness document for `") +
                   test_case.name + "`");
        if (from_cli != from_mcp) {
            std::cerr << "  cli: " << from_cli << "\n  mcp: " << from_mcp << "\n";
        }

        // Anti-vacuity: the case must diagnose on both sides, or the comparison above is "two empty
        // documents match" and proves nothing about a compiler.
        expect(diagnostic_count(from_cli) >= static_cast<std::size_t>(test_case.minimum),
               std::string("`") + test_case.name + "` reports the CLI's minimum diagnostics");
        expect(diagnostic_count(from_mcp) >= static_cast<std::size_t>(test_case.minimum),
               std::string("`") + test_case.name + "` reports the same through MCP");
    }

    // Determinism: the same call twice answers the same bytes, which is what lets a client diff two
    // runs and what the document spec promises (compiler-diagnostics-v1.md section 5).
    {
        const Case& test_case = cases[3];
        const std::string first =
            call_diagnostics(session, test_case.source, test_case.file, next_id++);
        const std::string second =
            call_diagnostics(session, test_case.source, test_case.file, next_id++);
        expect(!first.empty() && first == second, "two identical calls answer identical documents");
    }

    if (failures == 0) {
        std::cout << "mcp_diagnostics_parity: ok (" << sizeof(cases) / sizeof(cases[0])
                  << " cases)\n";
    }
    return failures == 0 ? 0 : 1;
}
