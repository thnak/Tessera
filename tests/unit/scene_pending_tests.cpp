// SPDX-License-Identifier: MIT

// The preview shows the Suspense fallback and the pending state (issue #657,
// docs/specs/pending-ui-v1.md sections 10 and 11, ADR-067): one TSX component with a typed
// suspending resource, a Suspense boundary, a transition-marked setter and a deferred value is
// compiled through the real front end and previewed with its resource faked by the session's
// doubles (what the Studio mock worker drives through `tessera_canvas_resource`). No browser, no
// clock: every step is a control or a click.

#include "codegen_pending_tsx_fixture.hpp"
#include "scene_pending.hpp"
#include "scene_session.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/ui/automation/scene_snapshot.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace fixture = tessera::test::pending_tsx;
namespace automation = tessera::ui::automation;
namespace ui = tessera::ui;

int g_failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_pending: " << message << "\n";
        ++g_failures;
    }
}

bool has_text(const ui::Node& node, std::string_view text) {
    const std::optional<ui::PaintData>& paint = node.paint_data();
    if (paint && paint->text == text) {
        return true;
    }
    for (const auto& child : node.children()) {
        if (has_text(*child, text)) {
            return true;
        }
    }
    return false;
}

std::uint64_t button_id(const cli::PreviewSession& session) {
    const auto snapshot = automation::snapshot_scene(session.root());
    if (snapshot) {
        for (const automation::UiSnapshotNode& node : snapshot->nodes) {
            if (node.kind == "Button") {
                return node.ui_id;
            }
        }
    }
    return 0;
}

cli::PendingControl control(cli::PendingAction action, std::string resource = "feed") {
    cli::PendingControl out;
    out.action = action;
    out.resource = std::move(resource);
    return out;
}

cli::PendingControl resolve(std::string text, std::uint64_t epoch = 0) {
    cli::PendingControl out = control(cli::PendingAction::Resolve);
    out.text = std::move(text);
    out.epoch = epoch;
    return out;
}

void run() {
    std::string error;
    const auto module = fixture::compile(fixture::kSource, error);
    expect(module.has_value(), "the fixture compiles: " + error);
    if (!module) {
        return;
    }
    const cli::ir::Component& component = module->components.back();
    const auto sheet =
        cli::compile_preview_stylesheet({}, cli::preview_media_context(320.0f, 240.0f, 1.0f));
    cli::PreviewSession session(*module, component, sheet, cli::PreviewSession::Options{});
    expect(session.built(), "the session builds: " + session.build_error());
    if (!session.built()) {
        return;
    }
    // The tree is replaced by every rebuild, so it is read through the session each time.
    const auto shows = [&session](std::string_view text) { return has_text(session.root(), text); };

    // The fetch the app starts on its first render is in flight and there is no data: the boundary
    // shows its fallback, not the content, and it owns no scene node of its own.
    cli::PendingReport report = session.pending_report();
    expect(report.active && report.resources.size() == 1 && report.boundaries.size() == 1,
           "one resource and one boundary are reported");
    expect(report.resources[0].status == "loading", "the resource opens in loading");
    expect(report.boundaries[0].fallback && !report.is_pending, "the boundary is in its fallback");
    expect(shows("Loading"), "the preview shows the fallback");
    expect(!shows("Updating"), "isPending is false before any transition");

    // The data restores the content.
    cli::PendingControlResult result = session.control_resource(resolve("hello"));
    expect(result.ok && result.accepted, "a completion for the current epoch lands");
    expect(!shows("Loading") && shows("hello"),
           "the content replaces the fallback and prints the data");
    expect(!session.pending_report().boundaries[0].fallback, "the boundary shows its content");

    // A marked setter: the transition starts, the host's fetch for the new key follows, and the
    // boundary keeps the content (Refreshing is not a suspension) while isPending shows.
    const std::uint64_t button = button_id(session);
    expect(button != 0, "the component has an addressable button");
    expect(session.click(button).found, "the click reaches the button");
    expect(session.built(), "the tree rebuilds after the click: " + session.build_error());
    report = session.pending_report();
    expect(report.is_pending && report.resources[0].status == "refreshing",
           "the marked setter is pending and the resource is refreshing");
    expect(!report.boundaries[0].fallback && report.boundaries[0].held,
           "the boundary holds its content");
    expect(shows("hello") && shows("Updating"),
           "the old data stays and the pending indicator shows");
    expect(shows("fast"), "the deferred value keeps the settled value while pending");

    // A completion for a superseded epoch is dropped and cannot clear the newer transition.
    expect(session.click(button).found, "a second marked change supersedes the first"); // epoch 3
    result = session.control_resource(resolve("stale", 2));
    expect(result.ok && !result.accepted, "a stale completion is dropped");
    expect(session.pending_report().is_pending && shows("Updating"),
           "the stale completion did not clear the transition");

    // The current one settles it.
    result = session.control_resource(resolve("world"));
    expect(result.accepted, "the current completion lands");
    report = session.pending_report();
    expect(!report.is_pending && report.resources[0].status == "success", "the transition settles");
    expect(shows("world") && !shows("Updating") && shows("slow"),
           "the new data, no indicator and the new deferred value show");

    // A failed refresh keeps the old data and clears nothing it should not.
    (void)session.control_resource(control(cli::PendingAction::Fetch));
    cli::PendingControl failure = control(cli::PendingAction::Fail);
    failure.message = "boom";
    expect(session.control_resource(failure).accepted, "a failure lands");
    report = session.pending_report();
    expect(report.resources[0].status == "error" && report.resources[0].error == "boom",
           "the status is error and the message is reported");
    expect(shows("world") && !report.boundaries[0].fallback, "a failed refresh keeps the old data");

    // Unknown names are reported, never applied.
    result = session.control_resource(control(cli::PendingAction::Fetch, "nope"));
    expect(!result.ok && !result.error.empty(), "an unknown resource is an error");

    // The page-facing JSON: one line, the same facts.
    const std::string json = cli::pending_control_json(&session, "fetch", "res::feed", 0, "", 0);
    expect(json.find("\"pendingVersion\":1") != std::string::npos &&
               json.find("\"changed\":true") != std::string::npos &&
               json.find("\"status\":\"refreshing\"") != std::string::npos &&
               json.find("\"fallback\":false") != std::string::npos &&
               json.find('\n') == json.size() - 1,
           "the control report is one line: " + json);
    const std::string unknown = cli::pending_control_json(&session, "explode", "feed", 0, "", 0);
    expect(unknown.find("\"ok\":false") != std::string::npos &&
               unknown.find("unknown resource action") != std::string::npos,
           "an unknown action is reported: " + unknown);
    const std::string none = cli::pending_control_json(nullptr, "fetch", "feed", 0, "", 0);
    expect(none.find("no canvas session") != std::string::npos, "no session is reported");
}

} // namespace

int main() {
    run();
    if (g_failures != 0) {
        std::cerr << "scene_pending: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_pending: ok\n";
    return 0;
}
