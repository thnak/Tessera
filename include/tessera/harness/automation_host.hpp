// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/command.hpp"
#include "tessera/harness/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace tessera::harness {

// Trace and audit documents are the host's bytes, not the harness's: the harness writes them
// unchanged so it never owns a second copy of either schema. The cap bounds what a script can
// make the process write (spec section 9, "reject traversal/oversized payloads").
inline constexpr std::size_t kMaxArtifactBytes = std::size_t{4} << 20U;

struct TraceArtifact {
    std::string json; // Chrome/Perfetto trace document
    std::uint64_t span_count = 0;
};

struct AuditArtifact {
    std::string json; // Lighthouse-style audit report
    double score = 0.0;
    bool passed = false;
};

// Everything the handlers need from the host runtime, as pure virtual calls. The harness library
// deliberately links no project target: a UI host, a test double, or a future `tessera headless`
// driver all satisfy this interface without the protocol layer knowing what a scene node is.
class AutomationHost {
public:
    AutomationHost() = default;
    AutomationHost(const AutomationHost&) = delete;
    AutomationHost& operator=(const AutomationHost&) = delete;
    virtual ~AutomationHost() = default;

    // The canonical UiSnapshot object produced by the automation serializer
    // (docs/specs/automation-ids-v1.md section 5.2). The harness embeds this value as-is: it never
    // computes, reorders, or rewrites UiIds, because identity belongs to the automation-id lane.
    // Returning a non-object is reported as Internal rather than encoded as a snapshot.
    [[nodiscard]] virtual std::expected<JsonValue, ProtocolError>
    canonical_snapshot(bool include_hidden) = 0;

    // Routes one decoded action through the host's normal input path. A target that does not
    // exist is UiIdNotFound; a step that cannot settle is Timeout. The handler echoes the action
    // rather than inventing a UI diff.
    [[nodiscard]] virtual std::expected<void, ProtocolError>
    dispatch_action(const ActionCommand& action) = 0;

    [[nodiscard]] virtual std::expected<TraceArtifact, ProtocolError> collect_trace() = 0;
    [[nodiscard]] virtual std::expected<AuditArtifact, ProtocolError>
    run_audit(std::string_view profile) = 0;
};

} // namespace tessera::harness
