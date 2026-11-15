// SPDX-License-Identifier: MIT

// The MCP lifecycle over the stdio loop: the handshake, revision negotiation, the initialized gate,
// and what a notification or an unknown method does. The loop is driven the way a client drives it
// -- bytes in, framed lines out -- through the harness's real `StdioTransport` over a scripted
// stream, so the framing rules (bounded line length, UTF-8, CRLF, EOF-terminated tail) are
// exercised too.

#include "mcp_test_support.hpp"
#include "session.hpp"
#include "stdio_test_support.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mcp = tessera::mcp;
namespace harness = tessera::harness;
using mcp_stdio_test::exchange;
using mcp_stdio_test::Fixture;
using mcp_stdio_test::line;
using mcp_test::decode;
using mcp_test::error_code;
using mcp_test::expect;
using mcp_test::string_at;

// The handshake line every lifecycle test starts from.
constexpr std::string_view kHandshake =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"studio","version":"0.1"}}})";

void test_initialize_and_list() {
    Fixture fixture;
    fixture->input = line(kHandshake) +
                     line(R"({"jsonrpc":"2.0","method":"notifications/initialized"})") +
                     line(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    mcp::McpSession session;
    const std::vector<std::string> responses = exchange(fixture, session, "mcp_stdio");
    expect(session.initialized(), "mcp_stdio: the handshake was accepted");

    // One line in, one line out, and the notification produced none: a server that answers a
    // notification breaks JSON-RPC clients that match by id.
    expect(responses.size() == 2, "mcp_stdio: two requests produced two responses and no third");
    if (responses.size() != 2) {
        return;
    }
    const harness::JsonValue init = decode(responses[0], "mcp_stdio");
    expect(string_at(init, "jsonrpc") == "2.0",
           "mcp_stdio: the response carries the jsonrpc version");
    const harness::JsonValue* result = init.find("result");
    expect(result != nullptr, "mcp_stdio: initialize answered with a result");
    if (result != nullptr) {
        expect(string_at(*result, "protocolVersion") == "2025-06-18",
               "mcp_stdio: the client's supported revision is negotiated");
        const harness::JsonValue* capabilities = result->find("capabilities");
        expect(capabilities != nullptr && capabilities->find("tools") != nullptr,
               "mcp_stdio: the tools capability is advertised");
        const harness::JsonValue* info = result->find("serverInfo");
        expect(info != nullptr && string_at(*info, "name") == "tessera-mcp",
               "mcp_stdio: the server identifies itself");
    }

    const harness::JsonValue listed = decode(responses[1], "mcp_stdio");
    const harness::JsonValue* list_result = listed.find("result");
    const harness::JsonValue* tools = list_result == nullptr ? nullptr : list_result->find("tools");
    expect(tools != nullptr && tools->as_array() != nullptr &&
               tools->as_array()->size() == mcp::tools().size(),
           "mcp_stdio: tools/list advertises the whole declared surface");
}

void test_unknown_revision_is_downgraded() {
    Fixture fixture;
    fixture->input = line(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2099-01-01"}})");
    mcp::McpSession session;
    const std::vector<std::string> responses = exchange(fixture, session, "mcp_stdio");
    expect(responses.size() == 1, "mcp_stdio: the handshake is answered");
    if (responses.empty()) {
        return;
    }
    const harness::JsonValue response = decode(responses.front(), "mcp_stdio");
    const harness::JsonValue* result = response.find("result");
    expect(result != nullptr &&
               string_at(*result, "protocolVersion") == std::string(mcp::kLatestProtocolVersion),
           "mcp_stdio: an unknown revision is answered with the server's latest");
}

void test_request_before_initialize() {
    Fixture fixture;
    // `ping` before the handshake is allowed (liveness), `tools/list` is not.
    fixture->input = line(R"({"jsonrpc":"2.0","id":1,"method":"ping"})") +
                     line(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    mcp::McpSession session;
    const std::vector<std::string> responses = exchange(fixture, session, "mcp_stdio");
    expect(responses.size() == 2, "mcp_stdio: both requests are answered");
    if (responses.size() != 2) {
        return;
    }
    expect(decode(responses[0], "mcp_stdio").find("result") != nullptr,
           "mcp_stdio: ping answers before the handshake");
    expect(error_code(decode(responses[1], "mcp_stdio")) ==
               static_cast<std::int64_t>(mcp::RpcErrorCode::NotInitialized),
           "mcp_stdio: a request before the handshake is refused with NotInitialized");
}

void test_notifications_and_unknown_method() {
    Fixture fixture;
    // An unknown notification is dropped silently; an unknown request gets MethodNotFound.
    fixture->input = line(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})") +
                     line(R"({"jsonrpc":"2.0","method":"notifications/cancelled"})") +
                     line(R"({"jsonrpc":"2.0","id":2,"method":"resources/list"})");
    mcp::McpSession session;
    const std::vector<std::string> responses = exchange(fixture, session, "mcp_stdio");
    expect(responses.size() == 2,
           "mcp_stdio: the notification was dropped and the request answered");
    if (responses.size() == 2) {
        expect(error_code(decode(responses[1], "mcp_stdio")) ==
                   static_cast<std::int64_t>(mcp::RpcErrorCode::MethodNotFound),
               "mcp_stdio: an unimplemented method is MethodNotFound");
    }
}

void test_malformed_line_is_answered() {
    Fixture fixture;
    // A malformed line must be answered with a parse error and the session must stay usable, so the
    // following request still gets its own answer: a client cannot recover from a silent drop.
    fixture->input = line("{ this is not json") +
                     line(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    mcp::McpSession session;
    const std::vector<std::string> responses = exchange(fixture, session, "mcp_stdio");
    expect(responses.size() == 2,
           "mcp_stdio: the malformed line and the request are both answered");
    if (responses.size() != 2) {
        return;
    }
    expect(error_code(decode(responses[0], "mcp_stdio")) ==
               static_cast<std::int64_t>(mcp::RpcErrorCode::ParseError),
           "mcp_stdio: the malformed line answered with ParseError");
    expect(decode(responses[1], "mcp_stdio").find("result") != nullptr,
           "mcp_stdio: the session stayed usable after a malformed line");
}

void test_crlf_and_eof_tail() {
    // A CRLF peer and a final line with no terminator (what an EOF-terminated stdin stream looks
    // like) must both work: the harness framer's rules, inherited by the adapter.
    Fixture fixture;
    fixture->input = std::string(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})") +
                     "\r\n" + R"({"jsonrpc":"2.0","id":2,"method":"ping"})"; // no trailing newline
    mcp::McpSession session;
    const std::vector<std::string> responses = exchange(fixture, session, "mcp_stdio");
    expect(responses.size() == 2,
           "mcp_stdio: a CRLF line and an EOF-terminated tail are both framed");
}

} // namespace

int main() {
    test_initialize_and_list();
    test_unknown_revision_is_downgraded();
    test_request_before_initialize();
    test_notifications_and_unknown_method();
    test_malformed_line_is_answered();
    test_crlf_and_eof_tail();
    if (mcp_test::failures == 0) {
        std::cout << "mcp_stdio: ok\n";
    }
    return mcp_test::failures == 0 ? 0 : 1;
}
