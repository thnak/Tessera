// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/artifact_sink.hpp"
#include "tessera/harness/automation_host.hpp"
#include "tessera/harness/command_handler.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace tessera::harness {

// Section 5 artifact names; an audit with no path writes `audit.json` in the output directory,
// and a trace resolves its required path against the same directory.
inline constexpr std::string_view kDefaultOutputDirectory = "out";
inline constexpr std::string_view kTraceArtifactName = "trace.json";
inline constexpr std::string_view kAuditArtifactName = "audit.json";

struct HarnessHandlerConfig {
    // Directory the section 5 default names are resolved in. Never taken from script input.
    std::string output_directory{kDefaultOutputDirectory};
    std::size_t max_artifact_bytes = kMaxArtifactBytes;
};

// The v1 command handler: snapshot consumes the host's canonical snapshot, actions go to the
// host's input sink, and trace/audit write host documents through an ArtifactSink under the
// documented names. Results echo what was dispatched; no command fabricates a UI state.
class HarnessCommandHandler final : public CommandHandler {
public:
    HarnessCommandHandler(AutomationHost& host, ArtifactSink& sink,
                          HarnessHandlerConfig config = {});
    ~HarnessCommandHandler() override = default;

    HarnessCommandHandler(const HarnessCommandHandler&) = delete;
    HarnessCommandHandler& operator=(const HarnessCommandHandler&) = delete;

    [[nodiscard]] std::expected<CommandResult, ProtocolError>
    handle(const CommandPayload& command) override;

private:
    [[nodiscard]] std::expected<CommandResult, ProtocolError>
    handle_snapshot(const SnapshotCommand& command);
    [[nodiscard]] std::expected<CommandResult, ProtocolError>
    handle_action(const ActionCommand& command);
    [[nodiscard]] std::expected<CommandResult, ProtocolError>
    handle_trace(const TraceCommand& command);
    [[nodiscard]] std::expected<CommandResult, ProtocolError>
    handle_audit(const AuditCommand& command);

    [[nodiscard]] std::string default_artifact_path(std::string_view name) const;

    AutomationHost& host_;
    ArtifactSink& sink_;
    HarnessHandlerConfig config_;
};

} // namespace tessera::harness
