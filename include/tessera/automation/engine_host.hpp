// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/app/host.hpp"
#include "tessera/harness/automation_host.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::diagnostics {
class MetricsRegistry;
class TraceRecorder;
enum class TraceCategory : std::uint8_t;
} // namespace tessera::diagnostics

namespace tessera::automation {

// Snapshot bounds for one live scene. They exist so a script cannot make the process copy an
// unbounded tree: the node cap bounds the builder's allocations, the depth cap bounds the recursion
// (and the structural path strings), and the path cap keeps each emitted path small.
inline constexpr std::size_t kMaxSnapshotNodes = 65536;
inline constexpr std::size_t kMaxSnapshotDepth = 64;
inline constexpr std::size_t kMaxSnapshotPathBytes = 256;

// Trace and audit attachment (docs/specs/headless-automation-v1.md sections 6.4 and 7).
//
// `enabled` gates every hook: while it is false the host never calls the recorder or the metrics
// registry, never serializes an artifact document, and pays only one predictable-branch test per
// command. Pointers may be attached before enabling, so a driver can toggle diagnostics without
// rebuilding the host. A disabled host answers `trace` with the empty Chrome document and reports
// `audit` as unavailable instead of fabricating a score.
struct AutomationDiagnostics {
    diagnostics::TraceRecorder* trace = nullptr;
    diagnostics::MetricsRegistry* metrics = nullptr;
    bool enabled = false;
};

// The concrete AutomationHost over the real engine: one scene root driven by an AppHost frame loop.
//
// Snapshots come from the automation snapshot builder over the live tree; click/type/focus/scroll
// go through the same `InputPump`/`InputRouter` path the window driver feeds, so
// hover/active/focus, click synthesis and the scroll default action are the production ones. After
// an action the host runs at most one settle frame (only when the input made a frame due), which is
// what lets a later snapshot observe applied state without the caller ticking the loop.
//
// Threading and cost: every call is synchronous on the UI thread, bounded by the snapshot caps and
// the pump's fixed queue, and never waits, sleeps, or spawns a task. The host borrows `root`,
// `host` and `loop`; they must outlive it.
class EngineAutomationHost final : public harness::AutomationHost {
public:
    struct Config {
        AutomationDiagnostics diagnostics{};
        std::size_t max_snapshot_nodes = kMaxSnapshotNodes;
    };

    EngineAutomationHost(ui::Node& root, app::AppHost& host, runtime::RuntimeLoop& loop,
                         Config config);
    // Defaults to an empty diagnostics attachment and the default snapshot cap. A separate
    // overload (instead of `Config = {}`) keeps the default argument from needing the nested
    // aggregate's default member initializers while the enclosing class is still incomplete, which
    // GCC and newer clang reject.
    EngineAutomationHost(ui::Node& root, app::AppHost& host, runtime::RuntimeLoop& loop);
    ~EngineAutomationHost() override = default;

    EngineAutomationHost(const EngineAutomationHost&) = delete;
    EngineAutomationHost& operator=(const EngineAutomationHost&) = delete;

    [[nodiscard]] std::expected<harness::JsonValue, harness::ProtocolError>
    canonical_snapshot(bool include_hidden) override;

    [[nodiscard]] std::expected<void, harness::ProtocolError>
    dispatch_action(const harness::ActionCommand& action) override;

    [[nodiscard]] std::expected<harness::TraceArtifact, harness::ProtocolError>
    collect_trace() override;

    [[nodiscard]] std::expected<harness::AuditArtifact, harness::ProtocolError>
    run_audit(std::string_view profile) override;

    // Replaces the hook attachment. Disabling drops no engine state, so a later enable records the
    // operations that happen after it.
    void set_diagnostics(AutomationDiagnostics diagnostics);
    [[nodiscard]] const AutomationDiagnostics& diagnostics() const noexcept;

    // The pump the host dispatches through; a driver may post extra input or inspect the router.
    [[nodiscard]] ui::InputPump& input() noexcept {
        return pump_;
    }

    // Deterministic counters (no wall clock): what the host did, not how long it took. The
    // diagnostics counters only move while diagnostics are enabled, which is what the no-cost test
    // asserts.
    struct Stats {
        std::uint64_t snapshots = 0;
        std::uint64_t actions = 0;
        std::uint64_t traces = 0;
        std::uint64_t audits = 0;
        std::uint64_t settle_frames = 0;
        std::uint64_t trace_events = 0;
        std::uint64_t audit_reports = 0;
        std::uint64_t emitted_nodes = 0; // node count of the last snapshot
    };

    [[nodiscard]] const Stats& stats() const noexcept {
        return stats_;
    }

private:
    [[nodiscard]] bool diagnostics_enabled() const noexcept {
        return config_.diagnostics.enabled;
    }

    // First visible node (preorder) whose automation id is `ui_id`; nullptr when absent. An
    // invisible subtree is not targetable, matching the snapshot's hidden-node policy.
    [[nodiscard]] ui::Node* find_target(harness::UiId ui_id) const;

    // Target of an action: `find_target` when `item_key` is empty, otherwise the first visible
    // realized node (preorder) carrying both `ui_id` and that key (engine_host_items.cpp). Errors
    // are `uiIdNotFound` for an absent/hidden/unrealized item and `invalidArgument` for a key that
    // is not canonical decimal text.
    [[nodiscard]] std::expected<ui::Node*, harness::ProtocolError>
    resolve_target(harness::UiId ui_id, const std::optional<std::string>& item_key) const;

    [[nodiscard]] std::expected<void, harness::ProtocolError> click(ui::Node& target);
    [[nodiscard]] std::expected<void, harness::ProtocolError> type_text(ui::Node& target,
                                                                        const std::string& text);
    [[nodiscard]] std::expected<void, harness::ProtocolError> focus(ui::Node& target);
    [[nodiscard]] std::expected<void, harness::ProtocolError> scroll(ui::Node& target, double dx,
                                                                     double dy);

    // Applies at most one frame when the dispatched input made one due (and no frame otherwise).
    void settle();

    // Next deterministic event timestamp: a fixed step per dispatched event, never a wall clock.
    [[nodiscard]] double next_event_seconds();

    void record(std::string_view name, diagnostics::TraceCategory category) const;

    ui::Node& root_;
    app::AppHost& host_;
    runtime::RuntimeLoop& loop_;
    ui::InputPump pump_;
    Config config_;
    mutable Stats stats_;
    std::uint64_t snapshot_sequence_ = 0;
    std::uint64_t event_count_ = 0;
};

} // namespace tessera::automation
