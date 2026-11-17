// SPDX-License-Identifier: MIT

// The tool surface (docs/specs/designer-studio-v1.md section 8): what `tools/list` advertises, and
// what a client gets when it calls a tool this build cannot run. Both are published, so both are
// asserted: an agent must be able to tell an implemented tool from a declared one without calling
// it, and a declared one must answer with a clean MCP error rather than a result that looks like
// success.

#include "mcp_protocol.hpp"
#include "mcp_test_support.hpp"
#include "session.hpp"
#include "tool_registry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mcp = tessera::mcp;
namespace harness = tessera::harness;
using mcp_test::call;
using mcp_test::expect;
using mcp_test::greet;
using mcp_test::result_is_error;
using mcp_test::result_text;
using mcp_test::string_at;

void test_surface_is_declared() {
    const std::vector<mcp::ToolDefinition>& tools = mcp::tools();
    // The section 8 table exactly: a tool missing here is one a client cannot discover, and an
    // extra one is a surface nobody decided on.
    const std::string_view expected[] = {
        "open_project", "list_files",  "diagnostics", "get_ir",     "apply_edits", "set_viewport",
        "snapshot",     "query_style", "pick",        "screenshot", "run_script"};
    expect(tools.size() == std::size(expected),
           "mcp_tools: the listing is exactly the section 8 tools");
    for (const std::string_view name : expected) {
        expect(mcp::find_tool(name) != nullptr,
               std::string("mcp_tools: the surface declares `") + std::string(name) + "`");
    }

    // Every tool publishes a description and an object schema with its properties. A tool without
    // either is unusable by a client that validates before calling.
    for (const mcp::ToolDefinition& tool : tools) {
        expect(!tool.description.empty(),
               std::string("mcp_tools: `") + std::string(tool.name) + "` has a description");
        expect(string_at(tool.input_schema, "type") == "object",
               std::string("mcp_tools: `") + std::string(tool.name) +
                   "` publishes an object schema");
        expect(tool.input_schema.find("properties") != nullptr,
               std::string("mcp_tools: `") + std::string(tool.name) + "` publishes its properties");
    }

    // The implemented subset, and nothing else claims to be. This is the assertion that keeps the
    // advertisement honest as later slices fill tools in: `open_project`, the three compile tools,
    // and since issue #408 the preview pair (`set_viewport` and `snapshot`), which share the scene
    // driver the browser module and the native preview CLI call.
    const std::string_view implemented[] = {"open_project", "list_files",   "diagnostics",
                                            "get_ir",       "set_viewport", "snapshot"};
    for (const mcp::ToolDefinition& tool : tools) {
        bool should_be_implemented = false;
        for (const std::string_view name : implemented) {
            should_be_implemented = should_be_implemented || tool.name == name;
        }
        expect((tool.status == mcp::ToolStatus::Implemented) == should_be_implemented,
               std::string("mcp_tools: `") + std::string(tool.name) + "` has the expected status");
    }
    expect(mcp::find_tool("no_such_tool") == nullptr,
           "mcp_tools: an unknown name has no definition");
}

void test_tools_list() {
    mcp::McpSession session;
    greet(session, "mcp_tools");
    auto reply = session.handle_line(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    expect(reply.has_value() && reply->answered, "mcp_tools: tools/list is answered");
    if (!reply) {
        return;
    }
    const harness::JsonValue response = mcp_test::decode(reply->reply, "mcp_tools");
    const harness::JsonValue* result = response.find("result");
    const harness::JsonValue* listed = result == nullptr ? nullptr : result->find("tools");
    const harness::JsonValue::Array* items = listed == nullptr ? nullptr : listed->as_array();
    expect(items != nullptr && items->size() == mcp::tools().size(),
           "mcp_tools: every declared tool is listed");
    if (items == nullptr || items->empty()) {
        return;
    }
    // Declaration order is stable and each entry carries its name, description, schema and status.
    for (std::size_t index = 0; index < items->size(); ++index) {
        const harness::JsonValue& entry = (*items)[index];
        expect(string_at(entry, "name") == mcp::tools()[index].name,
               "mcp_tools: the listing keeps the declaration order");
        expect(!string_at(entry, "description").empty(), "mcp_tools: each entry has a description");
        expect(entry.find("inputSchema") != nullptr, "mcp_tools: each entry has an input schema");
        const harness::JsonValue* annotations = entry.find("annotations");
        const harness::JsonValue* status =
            annotations == nullptr ? nullptr : annotations->find("tesseraStatus");
        expect(status != nullptr && status->is_string(),
               "mcp_tools: each entry publishes its status");
    }
    // Two calls answer the same bytes: a client may cache the listing.
    auto again = session.handle_line(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})");
    if (again) {
        const harness::JsonValue second = mcp_test::decode(again->reply, "mcp_tools");
        const harness::JsonValue* second_result = second.find("result");
        expect(second_result != nullptr && *second_result == *result,
               "mcp_tools: repeated listings are identical");
    }
}

void test_declared_tools_fail_cleanly() {
    mcp::McpSession session;
    greet(session, "mcp_tools");
    for (const mcp::ToolDefinition& tool : mcp::tools()) {
        if (tool.status != mcp::ToolStatus::Declared) {
            continue;
        }
        const harness::JsonValue response = call(session, tool.name, "{}", "mcp_tools");
        expect(response.find("error") == nullptr,
               std::string("mcp_tools: `") + std::string(tool.name) + "` is not a protocol error");
        expect(result_is_error(response),
               std::string("mcp_tools: `") + std::string(tool.name) + "` reports isError");
        const std::string text = result_text(response);
        expect(text.find(std::string(mcp::kUnimplementedToolCode)) != std::string::npos,
               std::string("mcp_tools: `") + std::string(tool.name) +
                   "` names the unimplemented code");
        expect(text.find(std::string(tool.name)) != std::string::npos,
               std::string("mcp_tools: `") + std::string(tool.name) +
                   "` names itself in the message");
    }
}

} // namespace

int main() {
    test_surface_is_declared();
    test_tools_list();
    test_declared_tools_fail_cleanly();
    if (mcp_test::failures == 0) {
        std::cout << "mcp_tools: ok\n";
    }
    return mcp_test::failures == 0 ? 0 : 1;
}
