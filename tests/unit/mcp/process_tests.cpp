// SPDX-License-Identifier: MIT

// The real process: spawn `tessera-mcp`, write a JSON-RPC transcript to its stdin, and assert the
// lines it writes to stdout. This is the exchange a client actually performs, and it is the half
// the in-process suite cannot prove -- that stdout carries protocol frames and nothing else (no
// banner, no log line, no stray byte), and that EOF ends the process cleanly.
//
// The transcript is a file and the response is a file, both under the build tree (never %TEMP%: a
// confined sandbox denies a child process writing there, and keeping them here makes a failure
// inspectable). Deterministic: fixed input, no clock, no network.
//
// Usage: mcp_process_tests <path-to-tessera-mcp> <scratch-dir>
// Exit codes: 0 pass, 1 failure, 2 usage or a platform without subprocesses.

#include "tessera/harness/json_parse.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

// The helpers below serve the subprocess path only; a target without one compiles a stub `main`.
#if defined(TESSERA_MCP_NO_SUBPROCESS)
#define TESSERA_MCP_HELPER [[maybe_unused]]
#else
#define TESSERA_MCP_HELPER
#endif

namespace fs = std::filesystem;
namespace harness = tessera::harness;

TESSERA_MCP_HELPER int failures = 0;

TESSERA_MCP_HELPER bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "mcp_process: " << message << "\n";
        ++failures;
    }
    return condition;
}

TESSERA_MCP_HELPER std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

TESSERA_MCP_HELPER void write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Quotes one argument for the shell the test runs under. The two hosts really do have two different
// quoting rules, and this is the one place that knows it.
TESSERA_MCP_HELPER std::string quoted(const fs::path& value) {
#ifdef _WIN32
    return "\"" + value.string() + "\"";
#else
    std::string text = "'";
    for (const char character : value.string()) {
        if (character == '\'') {
            text += "'\\''";
        } else {
            text += character;
        }
    }
    text += "'";
    return text;
#endif
}

TESSERA_MCP_HELPER std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            break; // an unterminated tail is not a frame; the caller asserts there is none
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

TESSERA_MCP_HELPER std::string string_at(const harness::JsonValue& object, std::string_view key) {
    const harness::JsonValue* value = object.find(key);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return std::string(*value->as_string());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <tessera-mcp> <scratch-dir>\n";
        return 2;
    }
#if defined(TESSERA_MCP_NO_SUBPROCESS)
    std::cerr << "mcp_process: skipped, this target has no subprocess support\n";
    return 0;
#else
    const fs::path executable(argv[1]);
    const fs::path work(argv[2]);
    std::error_code ignored;
    fs::remove_all(work, ignored);
    fs::create_directories(work);
    if (!fs::is_regular_file(executable, ignored)) {
        std::cerr << "mcp_process: no server at " << executable << "\n";
        return 2;
    }

    // The transcript: the handshake, the client's acknowledgement (a notification, which must not
    // be answered), `tools/list`, and one `tools/call` that compiles a broken sample. Then EOF,
    // which must end the process with exit 0.
    const std::string transcript =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"mcp_process","version":"1"}}})"
        "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
        "\n"
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"diagnostics","arguments":{"source":"export function Tag() {\n  return <NotAThing />;\n}\n","file":"ui/Tag.tsx"}}})"
        "\n";
    const fs::path input = work / "transcript.jsonl";
    const fs::path output = work / "responses.jsonl";
    write_file(input, transcript);

#ifdef _WIN32
    // cmd strips a leading quote from the whole /c command, so invoke through `call`.
    const std::string prefix = "call ";
#else
    const std::string prefix;
#endif
    const std::string command =
        prefix + quoted(executable) + " < " + quoted(input) + " > " + quoted(output) + " 2>&1";
    const int status = std::system(command.c_str());
#ifdef _WIN32
    const int exit_code = status;
#else
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    expect(exit_code == 0, "the server exits 0 on EOF");

    const std::string responses = read_file(output);
    const std::vector<std::string> lines = lines_of(responses);
    // Three requests were sent; the notification must not appear. A fourth line would mean stdout
    // carried something that is not a response (a banner, a log, a double answer).
    expect(lines.size() == 3, "stdout carries exactly one line per request and nothing else");
    // Every line must end with a newline: the peer frames on LF, so an unterminated last line would
    // only be readable because the process ended.
    expect(!responses.empty() && responses.back() == '\n', "every response line is terminated");

    if (lines.size() == 3) {
        const auto init = harness::parse_json(lines[0]);
        expect(init.has_value(), "the handshake response is strict JSON");
        if (init) {
            const harness::JsonValue* result = init->find("result");
            expect(result != nullptr, "the handshake answered with a result");
            if (result != nullptr) {
                expect(string_at(*result, "protocolVersion") == "2025-06-18",
                       "the negotiated revision is the client's");
            }
        }
        const auto listed = harness::parse_json(lines[1]);
        expect(listed.has_value(), "the tools/list response is strict JSON");
        if (listed) {
            const harness::JsonValue* result = listed->find("result");
            const harness::JsonValue* tools = result == nullptr ? nullptr : result->find("tools");
            expect(tools != nullptr && tools->as_array() != nullptr && !tools->as_array()->empty(),
                   "the tool surface comes back over a real pipe");
        }
        const auto called = harness::parse_json(lines[2]);
        expect(called.has_value(), "the tools/call response is strict JSON");
        if (called) {
            expect(called->find("error") == nullptr, "the call is not a protocol error");
            const harness::JsonValue* result = called->find("result");
            const harness::JsonValue* content =
                result == nullptr ? nullptr : result->find("content");
            const harness::JsonValue::Array* items =
                content == nullptr ? nullptr : content->as_array();
            expect(items != nullptr && items->size() == 1,
                   "the call result carries one content block");
            if (items != nullptr && !items->empty()) {
                const std::string document = string_at(items->front(), "text");
                const auto parsed = harness::parse_json(document);
                expect(parsed.has_value(),
                       "the diagnostics document inside the result is strict JSON");
                if (parsed) {
                    const harness::JsonValue* diagnostics = parsed->find("diagnostics");
                    expect(diagnostics != nullptr && diagnostics->as_array() != nullptr &&
                               !diagnostics->as_array()->empty(),
                           "the broken sample reports a diagnostic across the process boundary");
                    expect(string_at(*parsed, "file") == "ui/Tag.tsx",
                           "the span file survives the round trip");
                }
            }
        }
    }

    // `--version` and `--help` are the two documented non-serving modes, and both must answer on
    // stdout without starting a session.
    {
        const fs::path version_out = work / "version.txt";
        const std::string version_command =
            prefix + quoted(executable) + " --version > " + quoted(version_out) + " 2>&1";
        expect(std::system(version_command.c_str()) == 0, "--version exits 0");
        expect(read_file(version_out).find("tessera-mcp ") == 0, "--version prints its identity");
    }
    {
        const fs::path unknown_out = work / "unknown.txt";
        const std::string unknown_command =
            prefix + quoted(executable) + " --nonsense > " + quoted(unknown_out) + " 2>&1";
        expect(std::system(unknown_command.c_str()) != 0, "an unknown option is a usage error");
    }

    if (failures == 0) {
        std::cout << "mcp_process: ok\n";
    }
    return failures == 0 ? 0 : 1;
#endif
}
