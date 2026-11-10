// SPDX-License-Identifier: MIT

#include "tessera/harness/command.hpp"
#include "tessera/harness/command_result.hpp"
#include "tessera/harness/in_process_transport.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/harness/line_transport.hpp"
#include "tessera/harness/protocol.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace {

using tessera::harness::CommandPayload;
using tessera::harness::CommandResult;
using tessera::harness::decode_command;
using tessera::harness::decode_command_result;
using tessera::harness::decode_request;
using tessera::harness::decode_response;
using tessera::harness::encode_command;
using tessera::harness::encode_command_result;
using tessera::harness::encode_request;
using tessera::harness::encode_response;
using tessera::harness::InProcessPair;
using tessera::harness::JsonValue;
using tessera::harness::LineTransport;
using tessera::harness::make_error_response;
using tessera::harness::make_in_process_pair;
using tessera::harness::parse_json;
using tessera::harness::ProtocolErrorCode;
using tessera::harness::Request;
using tessera::harness::Response;
using tessera::harness::SnapshotCommand;
using tessera::harness::SnapshotResult;
using tessera::harness::to_json;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_golden: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string canonical(std::string_view line) {
    const auto parsed = parse_json(line);
    if (!parsed.has_value()) {
        return "<invalid json>";
    }
    const auto written = to_json(*parsed);
    return written.has_value() ? *written : std::string("<write failed>");
}

// A full client/server exchange over the in-process transport, from encoded request bytes back to
// decoded response. Returns the response line as the client read it.
std::optional<std::string> exchange(LineTransport& client, LineTransport& server,
                                    std::string_view request_line) {
    if (!client.write_line(request_line).has_value()) {
        return std::nullopt;
    }
    const auto received = server.read_line();
    if (!received.has_value() || *received != request_line) {
        return std::nullopt;
    }
    const auto request = decode_request(*received);
    if (!request.has_value()) {
        const Response failure = make_error_response(0, request.error());
        const auto encoded = encode_response(failure);
        if (!encoded.has_value() || !server.write_line(*encoded).has_value()) {
            return std::nullopt;
        }
        const auto reply = client.read_line();
        return reply.has_value() ? std::optional<std::string>(*reply) : std::nullopt;
    }
    const auto command = decode_command(*request);
    if (!command.has_value()) {
        const Response failure = make_error_response(request->id, command.error());
        const auto encoded = encode_response(failure);
        if (!encoded.has_value() || !server.write_line(*encoded).has_value()) {
            return std::nullopt;
        }
        const auto reply = client.read_line();
        return reply.has_value() ? std::optional<std::string>(*reply) : std::nullopt;
    }
    // A real executor supplies the snapshot tree; this fake is deterministic on purpose.
    const auto tree = parse_json(R"({"snapshotVersion":1,"sequence":"3","nodes":[]})");
    SnapshotResult result;
    result.include_hidden = std::get<SnapshotCommand>(*command).include_hidden;
    result.snapshot = *tree;
    const auto result_value = encode_command_result(CommandResult{std::move(result)});
    if (!result_value.has_value()) {
        return std::nullopt;
    }
    Response response;
    response.id = request->id;
    response.ok = true;
    response.result = *result_value;
    response.elapsed_us = 0;
    const auto encoded = encode_response(response);
    if (!encoded.has_value() || !server.write_line(*encoded).has_value()) {
        return std::nullopt;
    }
    const auto reply = client.read_line();
    return reply.has_value() ? std::optional<std::string>(*reply) : std::nullopt;
}

void check_golden_exchange() {
    InProcessPair pair = make_in_process_pair();
    const auto request_line = encode_command(1, SnapshotCommand{true});
    if (!expect(request_line.has_value(), "request encodes")) {
        return;
    }
    expect(*request_line ==
               R"({"protocolVersion":1,"id":"1","cmd":"snapshot","includeHidden":true})",
           "request golden bytes");
    expect(request_line->find('\n') == std::string::npos &&
               request_line->find('\r') == std::string::npos,
           "encoded line carries no terminator");

    const auto reply = exchange(*pair.first, *pair.second, *request_line);
    if (!expect(reply.has_value(), "exchange completes")) {
        return;
    }
    const std::string expected =
        R"({"protocolVersion":1,"id":"1","ok":true,"result":{"snapshot":{"snapshotVersion":1,"sequence":"3","nodes":[]},"includeHidden":true},"elapsedUs":0})";
    expect(*reply == expected, "response golden bytes");
    const auto response = decode_response(*reply);
    expect(response.has_value() && response->id == 1 && response->ok, "response correlates");
}

void check_error_exchange() {
    InProcessPair pair = make_in_process_pair();
    const auto reply =
        exchange(*pair.first, *pair.second, R"({"protocolVersion":1,"id":"42","cmd":"teleport"})");
    if (!expect(reply.has_value(), "error exchange completes")) {
        return;
    }
    expect(
        *reply ==
            R"({"protocolVersion":1,"id":"42","ok":false,"error":{"code":"unknownCommand","message":"unknown command","detail":"teleport"},"elapsedUs":0})",
        "unknown command error envelope bytes");
    const auto response = decode_response(*reply);
    expect(response.has_value() && !response->ok &&
               response->error.code == ProtocolErrorCode::UnknownCommand,
           "error envelope decodes");

    // A malformed line cannot be correlated, and the executor answers with id 0 without losing
    // framing.
    const auto bad = exchange(*pair.first, *pair.second, R"({"id":5,"cmd":)");
    expect(bad.has_value(), "malformed exchange completes");
    const auto bad_response = decode_response(*bad);
    expect(bad_response.has_value() && bad_response->id == 0 && !bad_response->ok,
           "malformed request answers with id 0");
}

void check_repeatability() {
    // The same value serializes to the same bytes every time; determinism is a property of the
    // codec, not of the process history.
    Request request;
    request.id = 99;
    request.command = "snapshot";
    request.arguments.set("includeHidden", JsonValue::boolean(true));
    const auto encoded = encode_request(request);
    if (!expect(encoded.has_value(), "repeat request encodes")) {
        return;
    }
    for (int iteration = 0; iteration < 64; ++iteration) {
        expect(encode_request(request).value_or("<failed>") == *encoded, "request bytes repeat");
    }

    Response response;
    response.id = 99;
    response.ok = true;
    response.result = JsonValue::object();
    response.elapsed_us = 1234;
    const auto response_text = encode_response(response);
    if (!expect(response_text.has_value(), "repeat response encodes")) {
        return;
    }
    for (int iteration = 0; iteration < 64; ++iteration) {
        expect(encode_response(response).value_or("<failed>") == *response_text,
               "response bytes repeat");
    }

    // Canonicalization is idempotent: formatting and field order are normalized once, then stable.
    const std::string loose = R"({ "cmd" : "snapshot" , "includeHidden" : true , "id" : 2 })";
    const std::string normalized = canonical(loose);
    expect(normalized == R"({"cmd":"snapshot","includeHidden":true,"id":2})",
           "first canonicalization");
    expect(canonical(normalized) == normalized, "canonicalization is idempotent");
    const auto decoded = decode_request(normalized);
    expect(decoded.has_value() && decoded->id == 2, "canonical form decodes");
    const auto reencoded = encode_request(*decoded);
    expect(reencoded.has_value() &&
               *reencoded ==
                   R"({"protocolVersion":1,"id":"2","cmd":"snapshot","includeHidden":true})",
           "re-encoding normalizes the envelope");
}

void check_unicode_and_large_payload() {
    // Non-ASCII text survives the encoder/decoder and transport without reordering or escaping
    // beyond what JSON requires.
    CommandPayload set_value =
        tessera::harness::ActionCommand{tessera::harness::ActionKind::SetValue, 5,   std::nullopt,
                                        "caf\xC3\xA9 \xF0\x9F\x98\x80",         0.0, 0.0};
    const auto encoded = encode_command(6, set_value);
    if (!expect(encoded.has_value(), "unicode command encodes")) {
        return;
    }
    InProcessPair pair = make_in_process_pair();
    expect(pair.first->write_line(*encoded).has_value(), "unicode write");
    const auto received = pair.second->read_line();
    expect(received.has_value() && *received == *encoded, "unicode round-trip over transport");

    // A snapshot result with many nodes still fits in one line: the transport cap is far above the
    // largest envelope the harness sends.
    JsonValue::Array nodes;
    for (int index = 0; index < 256; ++index) {
        JsonValue node = JsonValue::object();
        node.set("uiId", JsonValue::string(std::to_string(1000 + index)));
        node.set("kind", JsonValue::string("Button"));
        node.set("name", JsonValue::string("Action"));
        nodes.push_back(std::move(node));
    }
    JsonValue tree = JsonValue::object();
    tree.set("snapshotVersion", JsonValue::integer(1));
    tree.set("sequence", JsonValue::string("9"));
    tree.set("nodes", JsonValue::array(std::move(nodes)));
    SnapshotResult result;
    result.snapshot = std::move(tree);
    const auto result_value = encode_command_result(CommandResult{std::move(result)});
    if (!expect(result_value.has_value(), "large result encodes")) {
        return;
    }
    Response response;
    response.id = 7;
    response.ok = true;
    response.result = *result_value;
    const auto encoded_response = encode_response(response);
    expect(encoded_response.has_value(), "large response encodes");
    expect(encoded_response->size() < tessera::harness::kMaxLineBytes,
           "large response fits one line");
    const auto decoded = decode_response(*encoded_response);
    expect(decoded.has_value() && decoded->ok, "large response decodes");
    const auto decoded_result = decode_command_result(decoded->result, "snapshot");
    expect(decoded_result.has_value() &&
               std::get<SnapshotResult>(*decoded_result).snapshot.find("nodes")->size() == 256,
           "large result round-trips");
}

} // namespace

int main() {
    check_golden_exchange();
    check_error_exchange();
    check_repeatability();
    check_unicode_and_large_payload();
    if (failures != 0) {
        std::cerr << "harness_golden: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
