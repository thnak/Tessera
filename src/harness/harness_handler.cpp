// SPDX-License-Identifier: MIT

#include "tessera/harness/harness_handler.hpp"

#include <cmath>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace tessera::harness {
namespace {

ProtocolError internal_error(std::string message, std::string detail) {
    return ProtocolError{ProtocolErrorCode::Internal, std::move(message), std::move(detail),
                         std::nullopt};
}

ProtocolError invalid_path(std::string detail) {
    return ProtocolError{ProtocolErrorCode::InvalidArgument, "artifact path is not safe",
                         std::move(detail), std::nullopt};
}

std::unexpected<ProtocolError> unexpected(ProtocolError error) {
    return std::unexpected(std::move(error));
}

// The cap is checked before the sink so a host that produced an oversized document is reported as
// a command failure, not as a half-written file.
std::expected<void, ProtocolError> check_artifact(const std::string& bytes, std::size_t cap,
                                                  const std::string& path) {
    if (bytes.size() > cap) {
        return std::unexpected(internal_error("artifact exceeds the size cap",
                                              path + " (" + std::to_string(bytes.size()) + " > " +
                                                  std::to_string(cap) + " bytes)"));
    }
    return {};
}

} // namespace

HarnessCommandHandler::HarnessCommandHandler(AutomationHost& host, ArtifactSink& sink,
                                             HarnessHandlerConfig config)
    : host_(host),
      sink_(sink),
      config_(std::move(config)) {}

std::string HarnessCommandHandler::default_artifact_path(std::string_view name) const {
    if (config_.output_directory.empty()) {
        return std::string(name);
    }
    std::string path = config_.output_directory;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path.append(name);
    return path;
}

std::expected<CommandResult, ProtocolError>
HarnessCommandHandler::handle(const CommandPayload& command) {
    return std::visit(
        [this](const auto& value) -> std::expected<CommandResult, ProtocolError> {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, SnapshotCommand>) {
                return handle_snapshot(value);
            } else if constexpr (std::is_same_v<Type, ActionCommand>) {
                return handle_action(value);
            } else if constexpr (std::is_same_v<Type, TraceCommand>) {
                return handle_trace(value);
            } else {
                return handle_audit(value);
            }
        },
        command);
}

std::expected<CommandResult, ProtocolError>
HarnessCommandHandler::handle_snapshot(const SnapshotCommand& command) {
    auto snapshot = host_.canonical_snapshot(command.include_hidden);
    if (!snapshot.has_value()) {
        return unexpected(std::move(snapshot.error()));
    }
    if (!snapshot->is_object()) {
        return unexpected(internal_error("canonical snapshot must be a JSON object", "snapshot"));
    }
    return CommandResult{SnapshotResult{command.include_hidden, std::move(*snapshot)}};
}

std::expected<CommandResult, ProtocolError>
HarnessCommandHandler::handle_action(const ActionCommand& command) {
    auto dispatched = host_.dispatch_action(command);
    if (!dispatched.has_value()) {
        return unexpected(std::move(dispatched.error()));
    }
    return CommandResult{ActionResult{command.kind, command.ui_id, command.item_key}};
}

std::expected<CommandResult, ProtocolError>
HarnessCommandHandler::handle_trace(const TraceCommand& command) {
    const std::string path =
        command.path.empty() ? default_artifact_path(kTraceArtifactName) : command.path;
    if (!is_safe_artifact_path(path)) {
        return unexpected(invalid_path(path));
    }
    auto artifact = host_.collect_trace();
    if (!artifact.has_value()) {
        return unexpected(std::move(artifact.error()));
    }
    if (auto capped = check_artifact(artifact->json, config_.max_artifact_bytes, path);
        !capped.has_value()) {
        return unexpected(std::move(capped.error()));
    }
    if (auto written = sink_.write(path, artifact->json); !written.has_value()) {
        return unexpected(std::move(written.error()));
    }
    return CommandResult{TraceResult{path, artifact->span_count}};
}

std::expected<CommandResult, ProtocolError>
HarnessCommandHandler::handle_audit(const AuditCommand& command) {
    const std::string path =
        command.path.empty() ? default_artifact_path(kAuditArtifactName) : command.path;
    if (!is_safe_artifact_path(path)) {
        return unexpected(invalid_path(path));
    }
    auto artifact = host_.run_audit(command.profile);
    if (!artifact.has_value()) {
        return unexpected(std::move(artifact.error()));
    }
    if (!std::isfinite(artifact->score) || artifact->score < 0.0 || artifact->score > 100.0) {
        return unexpected(internal_error("audit score must be within 0..100", command.profile));
    }
    if (auto capped = check_artifact(artifact->json, config_.max_artifact_bytes, path);
        !capped.has_value()) {
        return unexpected(std::move(capped.error()));
    }
    if (auto written = sink_.write(path, artifact->json); !written.has_value()) {
        return unexpected(std::move(written.error()));
    }
    return CommandResult{AuditResult{command.profile, path, artifact->score, artifact->passed}};
}

} // namespace tessera::harness
