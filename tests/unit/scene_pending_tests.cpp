// SPDX-License-Identifier: MIT

// The preview shows the Suspense fallback and the pending state (issue #657,
// docs/specs/pending-ui-v1.md sections 10 and 11, ADR-067): one TSX component with a typed
// suspending resource, a Suspense boundary, a transition-marked setter and a deferred value is
// compiled through the real front end and previewed with its resource faked by the session's
// doubles (what the Studio mock worker drives through `tessera_canvas_resource`). No browser, no
// clock: every step is a control or a click.

#include "codegen_pending_tsx_fixture.hpp"
#include "scene_interaction.hpp"
#include "scene_pending.hpp"
#include "scene_project.hpp"
#include "scene_session.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/ui/automation/scene_snapshot.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <array>
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

// The `index`-th Button in tree order, or 0.
std::uint64_t button_id(const cli::PreviewSession& session, std::size_t index = 0) {
    const auto snapshot = automation::snapshot_scene(session.root());
    if (snapshot) {
        for (const automation::UiSnapshotNode& node : snapshot->nodes) {
            if (node.kind == "Button" && index-- == 0) {
                return node.ui_id;
            }
        }
    }
    return 0;
}

// The shared fixture with a second marked setter. A marked setter that assigns the value the cell
// already holds starts no transition, so superseding one needs a *different* value: the second
// button moves the mode to a third one, and the shared fixture (codegen_pending_tsx) stays as is.
std::string two_button_source() {
    std::string source = fixture::kSource;
    const auto replace = [&source](std::string_view from, std::string_view to) {
        const std::size_t at = source.find(from);
        if (at != std::string::npos) {
            source.replace(at, from.size(), to);
        }
    };
    replace("\"fast\" | \"slow\"", "\"fast\" | \"slow\" | \"slower\"");
    replace("      {isPending &&",
            "      <Button onClick={() => startTransition(() => setMode(\"slower\"))}>\n"
            "        <Text>Slower</Text>\n"
            "      </Button>\n"
            "      {isPending &&");
    return source;
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
    const std::string source = two_button_source();
    const auto module = fixture::compile(source, error);
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
    const std::uint64_t second = button_id(session, 1);
    expect(second != 0 && second != button, "the component has a second marked setter");
    expect(session.click(second).found, "a second marked change supersedes the first"); // epoch 3
    result = session.control_resource(resolve("stale", 2));
    expect(result.ok && !result.accepted, "a stale completion is dropped");
    expect(session.pending_report().is_pending && shows("Updating"),
           "the stale completion did not clear the transition");

    // The current one settles it.
    result = session.control_resource(resolve("world"));
    expect(result.accepted, "the current completion lands");
    report = session.pending_report();
    expect(!report.is_pending && report.resources[0].status == "success", "the transition settles");
    expect(shows("world") && !shows("Updating") && shows("slower"),
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

    // `status` is a read: the report, nothing applied, no frame owed.
    const std::string status = cli::pending_control_json(&session, "status", "", 0, "", 0);
    expect(status.find("\"ok\":true") != std::string::npos &&
               status.find("\"changed\":false") != std::string::npos &&
               status.find("\"status\":\"refreshing\"") != std::string::npos,
           "status reports the current state and changes nothing: " + status);
}

constexpr std::string_view kManifest =
    R"({"version":1,"services":[{"id":"demo.nav","name":"Navigator",)"
    R"("cpp":{"type":"demo::Navigator","header":"demo/services.hpp"},)"
    R"("methods":[{"name":"loadFeed","params":[{"name":"mode","type":"string"}],)"
    R"("returns":"string"}]}]})";

// The `resource` script step is the same control the canvas export takes, so the Wasm scene module
// (which has no GPU device) can exercise the vocabulary end to end.
void run_script() {
    const std::array<cli::ProjectFile, 1> sources{{{"ui/Feed.tsx", fixture::kSource}}};
    cli::ProjectInput project{sources, {}, "ui/Feed.tsx"};
    project.services_manifest = kManifest;
    const std::string document = cli::scene_input_document(
        project, cli::ScenePreviewOptions{}, "",
        "resource status\nresource resolve feed hello world\nresource resolve res::feed late @9\n"
        "resource fetch feed\nresource fail feed boom\n");
    const auto has = [&document](std::string_view part) {
        return document.find(part) != std::string::npos;
    };
    expect(has("\"kind\":\"resource\"") && has("\"status\":\"loading\""),
           "the first status step reports the opening fetch: " + document);
    expect(has("\"status\":\"success\"") && has("\"fallback\":false"),
           "a resolve restores the content");
    expect(has("\"accepted\":false"), "a stale epoch is dropped and reported");
    expect(has("\"status\":\"refreshing\"") && has("\"status\":\"error\"") &&
               has("\"error\":\"boom\""),
           "a refresh and a failure are reported");
    const std::string bad = cli::scene_input_document(project, cli::ScenePreviewOptions{}, "",
                                                      "resource explode feed\n");
    expect(bad.find("\"error\":\"line 1: `resource` needs") != std::string::npos,
           "an unknown action is a parse error: " + bad);
}

} // namespace

int main() {
    run();
    run_script();
    if (g_failures != 0) {
        std::cerr << "scene_pending: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_pending: ok\n";
    return 0;
}
