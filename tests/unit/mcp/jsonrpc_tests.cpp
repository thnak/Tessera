// SPDX-License-Identifier: MIT

// The JSON-RPC 2.0 envelope rules of the MCP binding (docs/specs/designer-studio-v1.md section 8,
// ADR-045 item 6). The codec is the whole protocol boundary: a client that sends a bad envelope
// must get a specification-correct error and an echoed id, and a client that sends a message this
// server cannot answer must be told so rather than left waiting.

#include "jsonrpc.hpp"
#include "mcp_protocol.hpp"
#include "tessera/harness/json_parse.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace harness = tessera::harness;
namespace mcp = tessera::mcp;

int failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "mcp_jsonrpc: " << message << "\n";
        ++failures;
    }
    return condition;
}

// Decodes an encoded line back into a value, so every assertion is about the wire bytes rather than
// about an in-memory shape that might not survive serialization.
harness::JsonValue decode(std::string_view line) {
    auto parsed = harness::parse_json(line);
    if (!parsed) {
        std::cerr << "mcp_jsonrpc: response is not strict JSON: " << line << "\n";
        ++failures;
        return {};
    }
    return *parsed;
}

std::string string_at(const harness::JsonValue& object, std::string_view key) {
    const harness::JsonValue* value = object.find(key);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return std::string(*value->as_string());
}

std::int64_t integer_at(const harness::JsonValue& object, std::string_view key) {
    const harness::JsonValue* value = object.find(key);
    if (value == nullptr) {
        return 0;
    }
    const auto number = value->as_integer();
    return number.has_value() ? *number : 0;
}

void test_request_decoding() {
    // A request: id, method, object params.
    {
        auto message =
            mcp::decode_message(R"({"jsonrpc":"2.0","id":7,"method":"tools/list","params":{}})");
        expect(message.has_value(), "a well-formed request decodes");
        if (message) {
            expect(message->id.present && !message->id.is_null(), "the id is present");
            expect(message->is_request(), "a message with an id is a request");
            expect(message->method == "tools/list", "the method is decoded");
            expect(message->params.is_object(), "object params are kept as an object");
            // The id is echoed by value, so a numeric id stays numeric (a client matching on 7 must
            // not receive "7").
            expect(message->id.value.as_integer() == std::optional<std::int64_t>(7),
                   "a numeric id keeps its number type");
        }
    }
    // A notification: no id at all.
    {
        auto message =
            mcp::decode_message(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        expect(message.has_value(), "a notification decodes");
        if (message) {
            expect(!message->id.present, "a notification has no id");
            expect(message->is_notification(), "and is classified as one");
        }
    }
    // A string id, and one above 2^53 sent as a string: no precision may be invented.
    {
        auto message =
            mcp::decode_message(R"({"jsonrpc":"2.0","id":"9007199254740993","method":"ping"})");
        expect(message.has_value(), "a string id decodes");
        if (message) {
            const auto id = message->id.value.as_string();
            expect(id.has_value() && *id == "9007199254740993",
                   "a uint64 id above 2^53 survives as text");
        }
    }
    // params may be omitted entirely.
    {
        auto message = mcp::decode_message(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
        expect(message.has_value() && message->params.is_object(),
               "omitted params read as an empty object");
    }
}

void test_rejections() {
    struct Case {
        std::string_view line;
        mcp::RpcErrorCode code;
        std::string_view why;
    };
    const Case cases[] = {
        {R"({"id":1,"method":"ping"})", mcp::RpcErrorCode::InvalidRequest,
         "a missing `jsonrpc` is an invalid request"},
        {R"({"jsonrpc":"1.0","id":1,"method":"ping"})", mcp::RpcErrorCode::InvalidRequest,
         "a wrong `jsonrpc` version is an invalid request"},
        {R"({"jsonrpc":"2.0","id":1})", mcp::RpcErrorCode::InvalidRequest,
         "a missing `method` is an invalid request"},
        {R"({"jsonrpc":"2.0","id":1,"method":""})", mcp::RpcErrorCode::InvalidRequest,
         "an empty `method` is an invalid request"},
        {R"({"jsonrpc":"2.0","id":true,"method":"ping"})", mcp::RpcErrorCode::InvalidRequest,
         "a boolean id is refused"},
        {R"({"jsonrpc":"2.0","id":1,"method":"ping","params":[1,2]})",
         mcp::RpcErrorCode::InvalidParams, "array params are refused"},
        {R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])", mcp::RpcErrorCode::InvalidRequest,
         "a JSON-RPC batch is refused, not half-executed"},
        {R"({"jsonrpc":"2.0","id":1,)", mcp::RpcErrorCode::ParseError,
         "truncated JSON is a parse error"},
        {R"(not json at all)", mcp::RpcErrorCode::ParseError, "a non-JSON line is a parse error"},
    };
    for (const Case& test_case : cases) {
        auto message = mcp::decode_message(test_case.line);
        const bool rejected = !message.has_value();
        expect(rejected, test_case.why);
        if (rejected) {
            expect(message.error().error.code == test_case.code, test_case.why);
        }
    }

    // The id is recovered when the envelope failed after it, so the client can correlate the error.
    {
        auto message = mcp::decode_message(R"({"jsonrpc":"2.0","id":42})");
        expect(!message.has_value(), "a request with no method is refused");
        if (!message) {
            const std::string line = mcp::encode_decode_failure(message.error());
            const harness::JsonValue response = decode(line);
            expect(integer_at(response, "id") == 42, "the recovered id is echoed on the error");
            expect(string_at(response, "jsonrpc") == "2.0",
                   "the error carries the jsonrpc version");
        }
    }
    // A parse error correlates with nothing, so it must carry a null id rather than a made-up one.
    {
        auto message = mcp::decode_message("{");
        expect(!message.has_value(), "a malformed line is refused");
        if (!message) {
            const harness::JsonValue response = decode(mcp::encode_decode_failure(message.error()));
            const harness::JsonValue* id = response.find("id");
            expect(id != nullptr && id->is_null(), "a parse error answers with a null id");
        }
    }
}

void test_encoding() {
    // A result: jsonrpc, id, result, in that order, and nothing else.
    {
        mcp::RpcId id;
        id.present = true;
        id.value = harness::JsonValue::integer(3);
        harness::JsonValue result = harness::JsonValue::object();
        result.set("ok", harness::JsonValue::boolean(true));
        auto line = mcp::encode_result(id, result);
        expect(line.has_value(), "a result encodes");
        if (line) {
            const harness::JsonValue response = decode(*line);
            expect(string_at(response, "jsonrpc") == "2.0",
                   "the result carries the jsonrpc version");
            expect(integer_at(response, "id") == 3, "the result echoes the id");
            expect(response.find("result") != nullptr, "the result member is present");
            expect(response.find("error") == nullptr, "a success carries no error");
            expect(line->find('\n') == std::string::npos,
                   "an encoded line carries no terminator (the transport owns it)");
        }
    }
    // An error: the numeric code, a message, and the typed data.code.
    {
        mcp::RpcId id;
        id.present = true;
        id.value = harness::JsonValue::string("abc");
        auto line = mcp::encode_error(
            id, mcp::RpcError{mcp::RpcErrorCode::MethodNotFound, "unknown method", ""});
        expect(line.has_value(), "an error encodes");
        if (line) {
            const harness::JsonValue response = decode(*line);
            const harness::JsonValue* error = response.find("error");
            expect(error != nullptr, "the error member is present");
            if (error != nullptr) {
                expect(integer_at(*error, "code") == -32601, "the error code is the JSON-RPC one");
                expect(string_at(*error, "message") == "unknown method", "the message is carried");
                expect(error->find("data") == nullptr, "an empty data code omits `data`");
            }
            expect(response.find("result") == nullptr, "a failure carries no result");
        }
    }
    // A notification id (absent) encodes as null in the envelope rather than as a fake 0.
    {
        const mcp::RpcId absent;
        auto line = mcp::encode_result(absent, harness::JsonValue::object());
        expect(line.has_value(), "a result with no id encodes");
        if (line) {
            const harness::JsonValue response = decode(*line);
            const harness::JsonValue* id = response.find("id");
            expect(id != nullptr && id->is_null(), "an absent id encodes as null");
        }
    }
    // The wire bytes are canonical: two encodes of one value are identical, which is what makes a
    // transcript reproducible for a test.
    {
        mcp::RpcId id;
        id.present = true;
        id.value = harness::JsonValue::integer(1);
        const auto first = mcp::encode_result(id, harness::JsonValue::object());
        const auto second = mcp::encode_result(id, harness::JsonValue::object());
        expect(first && second && *first == *second, "encoding is deterministic");
        expect(first && *first == R"({"jsonrpc":"2.0","id":1,"result":{}})",
               "the canonical bytes are the documented shape");
    }
}

void test_protocol_versions() {
    expect(mcp::protocol_version_supported("2025-06-18"), "2025-06-18 is supported");
    expect(mcp::protocol_version_supported("2024-11-05"), "2024-11-05 is supported");
    expect(!mcp::protocol_version_supported("1999-01-01"), "an unknown revision is not supported");
    expect(!mcp::protocol_version_supported(""), "an empty revision is not supported");
}

} // namespace

int main() {
    test_request_decoding();
    test_rejections();
    test_encoding();
    test_protocol_versions();
    if (failures == 0) {
        std::cout << "mcp_jsonrpc: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
