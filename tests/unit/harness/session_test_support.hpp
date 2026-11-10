// SPDX-License-Identifier: MIT

#pragma once

// Fixtures shared by the session tests. Everything here is deterministic: a manual clock, a
// recording handler, and transports whose stalls and EOF are scripted.

#include "tessera/harness/in_process_transport.hpp"
#include "tessera/harness/session.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace session_test {

using tessera::harness::ActionResult;
using tessera::harness::CommandHandler;
using tessera::harness::CommandPayload;
using tessera::harness::CommandResult;
using tessera::harness::InProcessPair;
using tessera::harness::JsonValue;
using tessera::harness::LineTransport;
using tessera::harness::make_in_process_pair;
using tessera::harness::ProtocolError;
using tessera::harness::RequestId;
using tessera::harness::Response;
using tessera::harness::SessionClock;
using tessera::harness::SessionExecutor;
using tessera::harness::SessionLimits;
using tessera::harness::SessionPumpStats;
using tessera::harness::SnapshotCommand;
using tessera::harness::SnapshotResult;
using tessera::harness::TraceResult;
using tessera::harness::TransportError;
using tessera::harness::TransportErrorCode;

inline int failures = 0;

inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_session: " << message << "\n";
        ++failures;
    }
    return condition;
}

class ManualSessionClock final : public SessionClock {
public:
    [[nodiscard]] std::uint64_t now_us() const override {
        return now;
    }

    std::uint64_t now = 0;
};

class RecordingHandler final : public CommandHandler {
public:
    [[nodiscard]] std::expected<CommandResult, ProtocolError>
    handle(const CommandPayload& command) override {
        seen.push_back(command);
        if (clock != nullptr) {
            clock->now += advance_us;
        }
        if (forced_error.has_value()) {
            return std::unexpected(*forced_error);
        }
        return std::visit(
            [this](const auto& value) -> std::expected<CommandResult, ProtocolError> {
                using Type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, SnapshotCommand>) {
                    if (oversized_snapshot_bytes != 0) {
                        return CommandResult{SnapshotResult{
                            value.include_hidden,
                            JsonValue::object({{"root", JsonValue::string(std::string(
                                                            oversized_snapshot_bytes, 'x'))}})}};
                    }
                    return CommandResult{SnapshotResult{value.include_hidden, snapshot}};
                } else if constexpr (std::is_same_v<Type, tessera::harness::ActionCommand>) {
                    return CommandResult{ActionResult{value.kind, value.ui_id, value.item_key}};
                } else if constexpr (std::is_same_v<Type, tessera::harness::TraceCommand>) {
                    return CommandResult{TraceResult{value.path, 2}};
                } else {
                    return CommandResult{
                        tessera::harness::AuditResult{value.profile, value.path, 90.0, true}};
                }
            },
            command);
    }

    ManualSessionClock* clock = nullptr;
    std::uint64_t advance_us = 0;
    std::optional<ProtocolError> forced_error;
    std::size_t oversized_snapshot_bytes = 0;
    JsonValue snapshot = JsonValue::object({{"root", JsonValue::object()}});
    std::vector<CommandPayload> seen;
};

inline std::string request_line(RequestId id, const CommandPayload& payload) {
    auto encoded = tessera::harness::encode_command(id, payload);
    if (!encoded.has_value()) {
        expect(false, "request encoding failed");
        return {};
    }
    return *encoded;
}

inline void write_lines(LineTransport& transport, const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        expect(transport.write_line(line).has_value(), "request line written");
    }
}

inline std::optional<Response> read_response(LineTransport& transport, const char* message) {
    auto line = transport.read_line();
    if (!line.has_value()) {
        expect(false, message);
        return std::nullopt;
    }
    auto response = tessera::harness::decode_response(*line);
    if (!response.has_value()) {
        expect(false, message);
        return std::nullopt;
    }
    return *response;
}

// Transport whose output can be stalled and whose input can reach EOF while output stays open,
// which the in-process pair cannot express (closing one endpoint closes both directions).
class StallingTransport final : public LineTransport {
public:
    [[nodiscard]] std::expected<std::string, TransportError> read_line() override {
        if (next_inbound < inbound.size()) {
            return inbound[next_inbound++];
        }
        if (input_done) {
            return std::unexpected(TransportError{TransportErrorCode::Closed, "input done"});
        }
        return std::unexpected(TransportError{TransportErrorCode::WouldBlock, "no line"});
    }

    [[nodiscard]] std::expected<void, TransportError> write_line(std::string_view line) override {
        if (closed_state) {
            return std::unexpected(TransportError{TransportErrorCode::Closed, "closed"});
        }
        if (block_writes) {
            return std::unexpected(TransportError{TransportErrorCode::WouldBlock, "stalled"});
        }
        outbound.emplace_back(line);
        return {};
    }

    [[nodiscard]] std::expected<void, TransportError> flush() override {
        if (block_writes) {
            return std::unexpected(TransportError{TransportErrorCode::WouldBlock, "stalled"});
        }
        return {};
    }

    [[nodiscard]] std::size_t max_line_bytes() const noexcept override {
        return 4096;
    }
    [[nodiscard]] bool closed() const noexcept override {
        return closed_state;
    }
    void close() noexcept override {
        closed_state = true;
    }

    std::vector<std::string> inbound;
    std::size_t next_inbound = 0;
    bool input_done = false;
    std::vector<std::string> outbound;
    bool block_writes = false;
    bool closed_state = false;
};

inline std::optional<Response> read_outbound(StallingTransport& transport) {
    if (transport.outbound.empty()) {
        expect(false, "expected an outbound response");
        return std::nullopt;
    }
    std::string line = std::move(transport.outbound.front());
    transport.outbound.erase(transport.outbound.begin());
    auto response = tessera::harness::decode_response(line);
    if (!response.has_value()) {
        expect(false, "outbound response did not decode");
        return std::nullopt;
    }
    return *response;
}

} // namespace session_test
