// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/eval/pending.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/ui/pending/fake_resource.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The preview's pending-UI support (issue #657, docs/specs/pending-ui-v1.md sections 10 and 11,
// ADR-067): the preview MUST show the fallback and the pending state, so every `useResource` of the
// previewed component is backed by a deterministic `FakeResource` double that the Studio mock
// worker (ADR-055) or a test drives through `PendingControl`.
//
// Nothing here is a second model of the rule. The doubles are the same header-only
// `tessera::ui::pending` types the evaluator and generated C++ run; this class is the wiring that
// gives a preview session one `PendingSession` over them and a vocabulary a page can call:
// `fetch`, `resolve` and `fail`, each optionally carrying the epoch the completion belongs to so a
// superseded completion can be shown being dropped.
//
// No clock, no randomness, no I/O and no thread: a resource changes only when a control (or a
// transition-marked handler, which starts the fetch the host would start) says so.
namespace tessera::compiler::codegen::cli {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;

// Whether the pending session answers this behavior instead of the registry: `useTransition` and
// `useDeferredValue` are model-backed (pending-ui-v1.md section 10.3), so their `beh::` outputs are
// read from the session and no engine instance is attached for them.
[[nodiscard]] inline bool pending_drives(const ir::Behavior& behavior) noexcept {
    return behavior.behavior == "transition" || behavior.behavior == "deferred_value";
}

// What a control does to one resource double.
enum class PendingAction : std::uint8_t {
    Fetch,   // a fetch starts: a key change or a refresh (the epoch moves, data is kept)
    Resolve, // a completion lands with a value
    Fail,    // a completion fails with a message; previous data is kept
};

// Parses `fetch`, `resolve` or `fail`. False for any other name (nothing is applied).
[[nodiscard]] bool pending_action_from(std::string_view name, PendingAction& out) noexcept;

struct PendingControl {
    PendingAction action = PendingAction::Fetch;
    // `res::<name>` or the bare `<name>`.
    std::string resource;
    // The epoch a completion belongs to; 0 means the resource's current one. A completion for any
    // other epoch is dropped (async-data-v1.md section 6) and reported as not accepted.
    std::uint64_t epoch = 0;
    // `Resolve`: the scalar of a number-typed resource, or the text of a string-typed one.
    double number = 0.0;
    std::string text;
    // `Fail`: the `.error` text.
    std::string message;
};

struct PendingControlResult {
    bool ok = false;       // the control named a known resource and a known action
    bool accepted = false; // the resource changed (false: a stale completion was dropped)
    std::string error;     // why `ok` is false
};

struct PendingResourceReport {
    std::string id; // `res::<name>`
    std::string status;
    std::uint64_t epoch = 0;
    std::string error;
};

struct PendingBoundaryReport {
    std::string id; // the `Suspense` IR node id
    bool fallback = false;
    bool held = false; // a pending transition keeps the content instead of the fallback
};

struct PendingReport {
    bool active = false; // the component declares at least one resource
    bool is_pending = false;
    std::vector<PendingResourceReport> resources;
    std::vector<PendingBoundaryReport> boundaries;
};

// The report `tessera_canvas_resource` returns to the page, as one JSON object on one line:
//
//   {"pendingVersion":1,"ok":true,"accepted":true,"changed":true,"isPending":false,
//    "resources":[{"id":"res::feed","status":"loading","epoch":1,"error":""}],
//    "boundaries":[{"id":"node::3","fallback":true,"held":false}],"error":""}
//
// `changed` equals `accepted`: an accepted control rebuilds the tree, so a frame is owed; a dropped
// stale completion changes nothing and costs none.
[[nodiscard]] std::string pending_report_json(const PendingControlResult& result,
                                              const PendingReport& report);

class PreviewSession;

// The whole of the canvas export: parse the action, apply the control to `session` (null: no
// running session) and report. An unknown action is reported, never applied. `epoch` 0 is the
// resource's current one.
[[nodiscard]] std::string pending_control_json(PreviewSession* session, std::string_view action,
                                               std::string_view resource, double number,
                                               std::string_view text, std::uint64_t epoch);

class PendingPreview final {
public:
    // Binds one double per resource of `component`, each with a fetch already in flight: the state
    // an app is in right after its first render, so the preview opens on the fallback. `component`
    // must outlive this object.
    explicit PendingPreview(const ir::Component& component);

    PendingPreview(const PendingPreview&) = delete;
    PendingPreview& operator=(const PendingPreview&) = delete;

    [[nodiscard]] eval::PendingSession& session() noexcept {
        return session_;
    }

    [[nodiscard]] PendingControlResult control(const PendingControl& control);

    // What a frame boundary does: the mark observes, the boundaries read the resources.
    void sync() noexcept {
        session_.sync();
    }

    // After a handler ran: a transition-marked setter started a transition that waits for the next
    // epoch of the resource it keys, and the host's fetch for the new key is what moves it. The
    // preview is that host, so it starts that fetch (the `epoch + 1` rule of spec section 10.2).
    void after_handler() noexcept;

    [[nodiscard]] PendingReport report() const;

private:
    [[nodiscard]] std::optional<std::size_t> find(std::string_view resource) const noexcept;

    const ir::Component& component_;
    eval::PendingSession session_;
    // Stable addresses: a `ResourceView` points at the double.
    std::deque<tessera::ui::pending::FakeResource> doubles_;
    // The text and the error a double publishes are views the double does not own, so each
    // resource keeps the bytes of its last completion here.
    std::vector<std::string> texts_;
    std::vector<std::string> errors_;
};

} // namespace tessera::compiler::codegen::cli
