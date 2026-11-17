// SPDX-License-Identifier: MIT

// The implemented tools of the compile-only surface, and the argument validation in front of them
// (docs/specs/designer-studio-v1.md section 8, slice D-S6). The `diagnostics` document's *content*
// is the parity test's job; this suite is about the dispatch contract around it: which channel a
// failure uses (a protocol error for bad arguments, a tool error for a call that ran and failed),
// and what `get_ir` returns where there is no IR to return.

#include "compile_tools.hpp"
#include "mcp_protocol.hpp"
#include "mcp_test_support.hpp"
#include "session.hpp"
#include "tool_session.hpp"

#include <string>
#include <string_view>

namespace {

namespace mcp = tessera::mcp;
namespace harness = tessera::harness;
using mcp_test::call;
using mcp_test::error_code;
using mcp_test::error_data_code;
using mcp_test::expect;
using mcp_test::greet;
using mcp_test::result_is_error;
using mcp_test::result_text;
using mcp_test::string_at;

constexpr std::string_view kCleanSource =
    R"({"source":"export function Clean() {\n  return <View />;\n}\n"})";
constexpr std::string_view kBrokenSource =
    R"({"source":"export function Tag() {\n  return <NotAThing />;\n}\n","file":"ui/Tag.tsx"})";

void test_diagnostics_tool() {
    mcp::McpSession session;
    greet(session, "mcp_compile");

    // A clean source: the spec's document with an empty array, and no tool error. A compile that
    // found nothing wrong is a success, not a missing document.
    {
        const harness::JsonValue response =
            call(session, "diagnostics", kCleanSource, "mcp_compile");
        expect(!result_is_error(response), "mcp_compile: a clean source is not a tool error");
        const harness::JsonValue document = mcp_test::decode(result_text(response), "mcp_compile");
        expect(document.find("diagnosticsVersion") != nullptr,
               "mcp_compile: the tool returns the versioned document");
        const harness::JsonValue* list = document.find("diagnostics");
        expect(list != nullptr && list->as_array() != nullptr && list->as_array()->empty(),
               "mcp_compile: a clean source reports no diagnostics");
        expect(string_at(document, "file") == std::string(mcp::kDefaultBufferName),
               "mcp_compile: the document names the default buffer when no `file` is given");
    }
    // A broken source: the document lists the error and the tool still succeeds. This mirrors the
    // CLI's contract, where a run with diagnostics exits 1 *and* writes the document.
    {
        const harness::JsonValue response =
            call(session, "diagnostics", kBrokenSource, "mcp_compile");
        expect(!result_is_error(response), "mcp_compile: a diagnostic is a result, not a failure");
        const harness::JsonValue document = mcp_test::decode(result_text(response), "mcp_compile");
        const harness::JsonValue* list = document.find("diagnostics");
        expect(list != nullptr && list->as_array() != nullptr && list->as_array()->size() == 1,
               "mcp_compile: the broken source reports one diagnostic");
        expect(string_at(document, "file") == "ui/Tag.tsx",
               "mcp_compile: the span file is the one the client sent");
    }
}

void test_get_ir_tool() {
    mcp::McpSession session;
    greet(session, "mcp_compile");

    // A clean source returns a module: the IR document, not a diagnostics document.
    {
        const harness::JsonValue response = call(session, "get_ir", kCleanSource, "mcp_compile");
        expect(!result_is_error(response), "mcp_compile: get_ir succeeds on a clean source");
        const harness::JsonValue module = mcp_test::decode(result_text(response), "mcp_compile");
        expect(module.find("components") != nullptr,
               "mcp_compile: the IR document carries its components");
        expect(module.find("diagnosticsVersion") == nullptr,
               "mcp_compile: and is not a diagnostics document");
    }
    // A broken source has no IR, so the tool reports the diagnostics with isError: a client asking
    // for IR must learn why there is none rather than receiving an empty module.
    {
        const harness::JsonValue response = call(session, "get_ir", kBrokenSource, "mcp_compile");
        expect(result_is_error(response), "mcp_compile: get_ir reports a lowerable failure");
        const harness::JsonValue document = mcp_test::decode(result_text(response), "mcp_compile");
        expect(document.find("diagnostics") != nullptr,
               "mcp_compile: and its error text is the diagnostics document");
    }
}

void test_open_project_tool() {
    mcp::McpSession session;
    greet(session, "mcp_compile");

    const harness::JsonValue response =
        call(session, "open_project", R"({"path":"."})", "mcp_compile");
    expect(!result_is_error(response), "mcp_compile: the working directory opens as a project");
    expect(session.state().project().has_value(),
           "mcp_compile: the session records the open project");

    // An empty path is refused as invalid *arguments*: the request itself is wrong, which is the
    // channel MCP draws for that, not a tool execution failure.
    const harness::JsonValue empty = call(session, "open_project", R"({"path":""})", "mcp_compile");
    expect(error_code(empty) == static_cast<std::int64_t>(mcp::RpcErrorCode::InvalidParams),
           "mcp_compile: an empty path is an invalid-params error");
    expect(error_data_code(empty) == std::string(mcp::kInvalidParamsCode),
           "mcp_compile: and the error names the argument");

    // A path that is not a directory is a tool execution error: the call was well formed and the
    // tool ran, it just could not do what it was asked.
    const harness::JsonValue missing =
        call(session, "open_project", R"({"path":"no/such/project/directory"})", "mcp_compile");
    expect(result_is_error(missing), "mcp_compile: a missing directory is a tool error");
    expect(session.state().project().has_value(),
           "mcp_compile: a refused open_project keeps the previously opened project");
}

void test_argument_validation() {
    mcp::McpSession session;
    greet(session, "mcp_compile");

    // An unknown tool is InvalidParams with a typed code: the client asked for something that is
    // not in the advertised surface, so blaming the request is correct.
    {
        const harness::JsonValue response = call(session, "no_such_tool", "{}", "mcp_compile");
        expect(error_data_code(response) == std::string(mcp::kUnknownToolCode),
               "mcp_compile: an unknown tool is a typed invalid-params error");
    }
    // An empty or non-string `source` names the argument, so an agent can fix the call from the
    // message alone. (No `source` at all is the folder-wide mode of issue #406, covered by
    // `mcp_project`.)
    for (const std::string_view arguments : {R"({"source":""})", R"({"source":7})"}) {
        const harness::JsonValue response = call(session, "diagnostics", arguments, "mcp_compile");
        const harness::JsonValue* error = response.find("error");
        expect(error != nullptr, "mcp_compile: an unusable `source` is refused");
        if (error != nullptr) {
            expect(string_at(*error, "message").find("`source`") != std::string::npos,
                   "mcp_compile: the message names the unusable argument");
        }
    }
    // A `file` that escapes the project has no project-relative span, so it is refused instead of
    // being resolved to a host path.
    {
        const harness::JsonValue response =
            call(session, "diagnostics",
                 R"({"source":"export function A() { return <View />; }","file":"../escape.tsx"})",
                 "mcp_compile");
        expect(response.find("error") != nullptr, "mcp_compile: a `..` file is refused");
    }
    // An over-sized source is refused with the typed size code rather than compiled as a prefix.
    {
        const std::string oversized(mcp::kMaxSourceBytes + 1, 'x');
        const std::string arguments = R"({"source":")" + oversized + R"("})";
        const harness::JsonValue response = call(session, "diagnostics", arguments, "mcp_compile");
        expect(error_data_code(response) == std::string(mcp::kInputTooLargeCode),
               "mcp_compile: an over-sized source is a typed size error");
    }
    // Non-object `arguments` is a protocol error, not a silent empty call.
    {
        const harness::JsonValue response =
            call(session, "diagnostics", R"(["not","an","object"])", "mcp_compile");
        expect(error_code(response) == static_cast<std::int64_t>(mcp::RpcErrorCode::InvalidParams),
               "mcp_compile: non-object arguments are refused");
    }
}

} // namespace

int main() {
    test_diagnostics_tool();
    test_get_ir_tool();
    test_open_project_tool();
    test_argument_validation();
    if (mcp_test::failures == 0) {
        std::cout << "mcp_compile_tools: ok\n";
    }
    return mcp_test::failures == 0 ? 0 : 1;
}
