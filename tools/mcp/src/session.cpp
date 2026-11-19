// SPDX-License-Identifier: MIT

#include "compile_tools.hpp"
#include "mcp_protocol.hpp"
#include "project_tools.hpp"
#include "query_tools.hpp"
#include "session.hpp"
#include "tessera/harness/json_write.hpp"
#include "tool_registry.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace tessera::mcp {
namespace {

using tessera::harness::to_json;

// A project path is a host path, not a source file: 4 KiB is far beyond any real path and bounded
// so a client cannot make the session hold an arbitrary string in its state.
constexpr std::size_t kMaxPathBytes = 4096;

std::string_view string_member(const JsonValue& object, std::string_view key) {
    const JsonValue* value = object.find(key);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return *value->as_string();
}

// A finite number argument, given as a JSON number or as numeric text (the viewport tool's
// arguments are text, so a client that quotes its numbers works for every tool). Refused rather
// than coerced: a typo must not become 0,0.
std::optional<float> number_value(const JsonValue* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    double parsed = 0.0;
    if (const std::optional<double> number = value->as_number(); number.has_value()) {
        parsed = *number;
    } else if (value->is_string() && !value->as_string()->empty() &&
               value->as_string()->size() <= 32) {
        const std::string_view text = *value->as_string();
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (error != std::errc{} || end != text.data() + text.size()) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (!std::isfinite(parsed)) {
        return std::nullopt;
    }
    return static_cast<float>(parsed);
}

// A `uiId`: a decimal string (ids above 2^53 do not survive a JSON number, automation-ids-v1
// section 1) or a non-negative integer that does.
std::optional<std::uint64_t> ui_id_value(const JsonValue* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (const std::optional<std::int64_t> integer = value->as_integer();
        integer.has_value() && *integer >= 0) {
        return static_cast<std::uint64_t>(*integer);
    }
    if (!value->is_string() || value->as_string()->empty() || value->as_string()->size() > 20) {
        return std::nullopt;
    }
    const std::string_view text = *value->as_string();
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return parsed;
}

// The `arguments` object of a `tools/call`. A tool with no arguments may omit it, and the schema
// `tools/list` publishes for such a tool is an empty object, so an omission and `{}` are the same
// request. Anything else is refused instead of being coerced.
std::expected<const JsonValue*, RpcError> tool_arguments(const JsonValue& params) {
    static const JsonValue kNoArguments = JsonValue::object();
    const JsonValue* arguments = params.find("arguments");
    if (arguments == nullptr) {
        return &kNoArguments;
    }
    if (!arguments->is_object()) {
        return std::unexpected(RpcError{RpcErrorCode::InvalidParams,
                                        "`arguments` must be an object", kInvalidParamsCode});
    }
    return arguments;
}

// A required non-empty string argument. The message names the argument so an agent can correct the
// call without re-reading the tool description; it is a caller-supplied literal because RpcError
// holds views and must not point into a temporary.
std::expected<std::string_view, RpcError>
string_argument(const JsonValue& arguments, std::string_view name, std::string_view message) {
    const JsonValue* value = arguments.find(name);
    if (value == nullptr || !value->is_string() || value->as_string()->empty()) {
        return std::unexpected(RpcError{RpcErrorCode::InvalidParams, message, kInvalidParamsCode});
    }
    return *value->as_string();
}

} // namespace

JsonValue initialize_result(std::string_view requested_version) {
    const std::string_view version =
        protocol_version_supported(requested_version) ? requested_version : kLatestProtocolVersion;

    JsonValue tools_capability = JsonValue::object();
    // `listChanged` is false and honest: the surface is fixed for the process lifetime, so this
    // server never sends `notifications/tools/list_changed`.
    tools_capability.set("listChanged", JsonValue::boolean(false));
    JsonValue capabilities = JsonValue::object();
    capabilities.set("tools", std::move(tools_capability));

    JsonValue info = JsonValue::object();
    info.set("name", JsonValue::string(std::string(kServerName)));
    info.set("version", JsonValue::string(std::string(kServerVersion)));

    JsonValue result = JsonValue::object();
    result.set("protocolVersion", JsonValue::string(std::string(version)));
    result.set("capabilities", std::move(capabilities));
    // An extension member, ignored by a client that does not know it: the tool surface has its own
    // version, because the protocol revision says nothing about which tools exist or how their
    // arguments are read (docs/specs/mcp-server-v1.md section 3).
    result.set("toolSurfaceVersion", JsonValue::integer(kToolSurfaceVersion));
    result.set("serverInfo", std::move(info));
    // The section 8 adapter is a tool surface only: no prompts, no resources, no sampling.
    result.set(
        "instructions",
        JsonValue::string(
            "Tessera compile-only adapter. `list_files`, `diagnostics` and `get_ir` read the "
            "project folder (an explicit `open_project`, else your first root, else the "
            "server's working directory) and run the real compiler front end over it; with "
            "`source` they compile that text in memory instead. Every tool that is not "
            "implemented is declared in `tools/list` and answers with "
            "TESSERA_MCP_TOOL_UNIMPLEMENTED."));
    return result;
}

std::expected<JsonValue, RpcError> McpSession::initialize(const JsonValue& params) {
    // The client's `roots` capability decides whether this server asks for its roots after the
    // handshake; without it the working directory is the root.
    const JsonValue* capabilities = params.find("capabilities");
    state_.set_client_roots(capabilities != nullptr && capabilities->find("roots") != nullptr);
    // A client that sends no version at all is answered with this server's newest one rather than
    // refused: the handshake is the first line of a session and being strict there helps nobody.
    return initialize_result(string_member(params, "protocolVersion"));
}

std::expected<JsonValue, RpcError> McpSession::call_tool(const JsonValue& params) {
    const std::string_view name = string_member(params, "name");
    if (name.empty()) {
        return std::unexpected(RpcError{RpcErrorCode::InvalidParams,
                                        "`name` must be a non-empty string", kInvalidParamsCode});
    }
    const ToolDefinition* tool = find_tool(name);
    if (tool == nullptr) {
        return std::unexpected(
            RpcError{RpcErrorCode::InvalidParams, "no tool with that name", kUnknownToolCode});
    }
    auto arguments = tool_arguments(params);
    if (!arguments) {
        return std::unexpected(arguments.error());
    }

    if (tool->status == ToolStatus::Declared) {
        // An execution error, not a protocol error: the request was well formed and the tool is a
        // real member of the surface. Answering `InvalidParams` would blame the client for asking
        // for something the server advertised.
        return tool_result_json(unimplemented_tool_result(*tool));
    }

    if (name == "open_project") {
        auto path = string_argument(**arguments, "path", "`path` must be a non-empty string");
        if (!path) {
            return std::unexpected(path.error());
        }
        if (path->size() > kMaxPathBytes) {
            return std::unexpected(
                RpcError{RpcErrorCode::InvalidParams, "`path` is too long", kInputTooLargeCode});
        }
        if (auto reason = state_.open_project(std::string(*path)); reason.has_value()) {
            return tool_result_json(ToolResult{true, std::move(*reason)});
        }
        return tool_result_json(ToolResult{false, "project opened: " + std::string(*path)});
    }

    if (name == "set_viewport") {
        // Every field is optional: `viewport.set` is "omitted fields keep their value", so a client
        // changing only the posture does not have to resend the size (docs/specs/
        // designer-protocol-v1.md section 6).
        return tool_result_json(run_set_viewport_tool(
            state_.viewport(), string_member(**arguments, "width"),
            string_member(**arguments, "height"), string_member(**arguments, "dpr"),
            string_member(**arguments, "foldPosture")));
    }

    if (name == "snapshot") {
        auto source = string_argument(**arguments, "source", "`source` must be a non-empty string");
        if (!source) {
            return std::unexpected(source.error());
        }
        if (source->size() > kMaxSourceBytes) {
            return std::unexpected(
                RpcError{RpcErrorCode::InvalidParams,
                         "`source` exceeds the 1 MiB per-file limit of docs/specs/jsx-subset-v1.md "
                         "section 15",
                         kInputTooLargeCode});
        }
        return tool_result_json(
            run_snapshot_tool(state_.viewport(), *source, string_member(**arguments, "file")));
    }

    if (name == "query_style" || name == "pick") {
        auto source = string_argument(**arguments, "source", "`source` must be a non-empty string");
        if (!source) {
            return std::unexpected(source.error());
        }
        if (source->size() > kMaxSourceBytes) {
            return std::unexpected(
                RpcError{RpcErrorCode::InvalidParams,
                         "`source` exceeds the 1 MiB per-file limit of docs/specs/jsx-subset-v1.md "
                         "section 15",
                         kInputTooLargeCode});
        }
        const std::string_view file = string_member(**arguments, "file");
        if (name == "query_style") {
            const std::optional<std::uint64_t> ui_id = ui_id_value((*arguments)->find("uiId"));
            if (!ui_id) {
                return std::unexpected(
                    RpcError{RpcErrorCode::InvalidParams,
                             "`uiId` must be a decimal string or a non-negative integer",
                             kInvalidParamsCode});
            }
            return tool_result_json(run_query_style_tool(state_.viewport(), *source, file, *ui_id));
        }
        const std::optional<float> x = number_value((*arguments)->find("x"));
        const std::optional<float> y = number_value((*arguments)->find("y"));
        if (!x || !y) {
            return std::unexpected(RpcError{RpcErrorCode::InvalidParams,
                                            "`x` and `y` must be finite numbers",
                                            kInvalidParamsCode});
        }
        return tool_result_json(run_pick_tool(state_.viewport(), *source, file, *x, *y));
    }

    if (name == "list_files") {
        return tool_result_json(run_list_files_tool(state_));
    }

    if (name == "generate") {
        // `write` is a boolean and `path` a string, both optional; a wrong type is refused rather
        // than read as the default, because `write` decides whether the disk changes.
        bool write = false;
        if (const JsonValue* flag = (*arguments)->find("write"); flag != nullptr) {
            const std::optional<bool> value = flag->as_bool();
            if (!value) {
                return std::unexpected(RpcError{RpcErrorCode::InvalidParams,
                                                "`write` must be a boolean", kInvalidParamsCode});
            }
            write = *value;
        }
        const JsonValue* path = (*arguments)->find("path");
        if (path != nullptr && !path->is_string()) {
            return std::unexpected(RpcError{RpcErrorCode::InvalidParams, "`path` must be a string",
                                            kInvalidParamsCode});
        }
        return tool_result_json(run_generate_tool(
            state_, write, path == nullptr ? std::string_view() : *path->as_string()));
    }

    if ((name == "diagnostics" || name == "get_ir") && (*arguments)->find("source") == nullptr) {
        // No `source`: the call is about the opened project folder, compiled as a whole the way the
        // native driver does. A call that sends `source` keeps the in-memory behaviour below.
        return tool_result_json(name == "diagnostics" ? run_project_diagnostics_tool(state_)
                                                      : run_project_ir_tool(state_));
    }

    if (name == "diagnostics" || name == "get_ir") {
        auto source = string_argument(**arguments, "source", "`source` must be a non-empty string");
        if (!source) {
            return std::unexpected(source.error());
        }
        if (source->size() > kMaxSourceBytes) {
            return std::unexpected(
                RpcError{RpcErrorCode::InvalidParams,
                         "`source` exceeds the 1 MiB per-file limit of docs/specs/jsx-subset-v1.md "
                         "section 15",
                         kInputTooLargeCode});
        }
        const std::string file = resolve_buffer_name(state_, string_member(**arguments, "file"));
        if (file.empty()) {
            return std::unexpected(
                RpcError{RpcErrorCode::InvalidParams,
                         "`file` must stay inside the project (no absolute path or `..`)",
                         kInvalidParamsCode});
        }
        const ToolResult result = name == "diagnostics" ? run_diagnostics_tool(*source, file)
                                                        : run_get_ir_tool(*source, file);
        return tool_result_json(result);
    }

    // Unreachable by construction: ToolStatus::Implemented is exactly the set handled above, and
    // the registry test asserts that. Answered rather than asserted so a future tool cannot crash a
    // session that is already serving a client.
    return tool_result_json(unimplemented_tool_result(*tool));
}

std::string McpSession::next_roots_request() {
    pending_roots_id_ = "tessera-roots-" + std::to_string(++roots_requests_);
    JsonValue request = JsonValue::object();
    request.set("jsonrpc", JsonValue::string(std::string(kJsonRpcVersion)));
    request.set("id", JsonValue::string(pending_roots_id_));
    request.set("method", JsonValue::string("roots/list"));
    auto text = to_json(request);
    return text ? std::move(*text) : std::string();
}

void McpSession::handle_roots_response(const IncomingMessage& message) {
    if (pending_roots_id_.empty() || !message.id.value.is_string() ||
        *message.id.value.as_string() != pending_roots_id_) {
        return;
    }
    pending_roots_id_.clear();
    std::vector<std::string> uris;
    const JsonValue* roots = message.result.find("roots");
    if (roots != nullptr && roots->is_array()) {
        for (const JsonValue& root : *roots->as_array()) {
            const JsonValue* uri = root.find("uri");
            if (uri != nullptr && uri->is_string()) {
                uris.emplace_back(*uri->as_string());
            }
        }
    }
    state_.apply_roots(uris);
}

std::expected<SessionReply, std::string_view>
McpSession::handle_message(const IncomingMessage& message) {
    if (message.is_response) {
        // The only request this server issues is `roots/list`; anything else is not ours.
        handle_roots_response(message);
        return SessionReply{};
    }
    const bool is_request = message.is_request();
    const auto respond = [&message, is_request](std::expected<JsonValue, RpcError> result)
        -> std::expected<SessionReply, std::string_view> {
        if (!is_request) {
            // A notification is never answered; an unknown or failed notification is dropped, as
            // JSON-RPC requires (there is no id to correlate an error with).
            return SessionReply{};
        }
        if (!result) {
            auto text = encode_error(message.id, result.error());
            if (!text) {
                return std::unexpected(text.error());
            }
            return SessionReply{std::move(*text), true, {}};
        }
        auto text = encode_result(message.id, *result);
        if (!text) {
            return std::unexpected(text.error());
        }
        return SessionReply{std::move(*text), true, {}};
    };

    if (!message.params_were_object) {
        return respond(std::unexpected(RpcError{RpcErrorCode::InvalidParams,
                                                "`params` must be an object", kInvalidParamsCode}));
    }

    if (message.method == "initialize") {
        auto result = initialize(message.params);
        if (result) {
            initialized_ = true;
        }
        return respond(std::move(result));
    }
    if (message.method == "ping") {
        // Liveness only, and deliberately allowed before the handshake: a client probing a server
        // it has not initialized yet should get an answer, not a lifecycle error.
        return respond(JsonValue::object());
    }
    if (message.method == "notifications/initialized" ||
        message.method == "notifications/roots/list_changed") {
        // The client's acknowledgement of the handshake (never answered), or its roots changed.
        // A client that declared the `roots` capability is asked for them, once per event; the
        // answer re-resolves the project unless the client opened one explicitly.
        SessionReply reply;
        if (initialized_ && state_.client_roots()) {
            if (std::string request = next_roots_request(); !request.empty()) {
                reply.outbound.push_back(std::move(request));
            }
        }
        if (is_request) {
            return respond(JsonValue::object());
        }
        return reply;
    }
    if (!initialized_) {
        return respond(std::unexpected(RpcError{RpcErrorCode::NotInitialized,
                                                "send `initialize` before any other method",
                                                kNotInitializedCode}));
    }
    if (message.method == "tools/list") {
        return respond(tools_list_result());
    }
    if (message.method == "tools/call") {
        return respond(call_tool(message.params));
    }
    return respond(std::unexpected(
        RpcError{RpcErrorCode::MethodNotFound, "unknown method", kInvalidParamsCode}));
}

std::expected<SessionReply, std::string_view> McpSession::handle_line(std::string_view line) {
    auto message = decode_message(line);
    if (!message) {
        return SessionReply{encode_decode_failure(message.error()), true, {}};
    }
    return handle_message(*message);
}

} // namespace tessera::mcp
