// SPDX-License-Identifier: MIT
//
// The host half of the dev-mode compiler-error overlay (issue #78 item 4): the panel mounts through
// a real OverlayHost over a real AppHost, so the layer, dismissal and "one updating panel, not a
// pile" behaviour is the shipped one rather than a mock.
//
// The document parse and the rendered lines are covered by `compiler_error_overlay_document`; the
// literals both files share live in compiler_error_overlay_docs.hpp.

#include "compiler_error_overlay_docs.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/boundary/report.hpp"
#include "tessera/ui/devtools/compiler_error_overlay.hpp"
#include "tessera/ui/overlay/overlay.hpp"
#include "tessera/ui/scene/node.hpp"

#include <iostream>
#include <memory>
#include <string>

namespace {

namespace devtools = tessera::ui::devtools;
using tessera::test::compiler_error_overlay::check;
using tessera::test::compiler_error_overlay::failures;
using tessera::test::compiler_error_overlay::kCleanDocument;
using tessera::test::compiler_error_overlay::kErrorDocument;
using tessera::test::compiler_error_overlay::kTwoErrorDocument;
using tessera::test::compiler_error_overlay::kWarningDocument;
using tessera::ui::Node;
using tessera::ui::OverlayHost;
using tessera::ui::OverlayLayer;

// A 400x800 window with one base content area, so layout, style and hit testing come from the
// shipped pipeline.
struct Fixture {
    Fixture()
        : root(1),
          loop(clock) {
        Node& base = root.add_child(std::make_unique<Node>(2));
        base.set_element_kind("View");
        base.set_class_name("fill");
        tessera::app::HostConfig config;
        config.width = 400.0f;
        config.height = 800.0f;
        host = std::make_unique<tessera::app::AppHost>(
            root,
            tessera::ui::style::compile_stylesheet(
                ".fill { flex-grow: 1; display: flex; flex-direction: column; }"
                ".tessera-compiler-error-line { width: 100%; height: 18px; }"),
            loop, config);
        overlays = &host->overlays();
    }

    tessera::runtime::ManualClock clock;
    Node root;
    tessera::runtime::RuntimeLoop loop;
    std::unique_ptr<tessera::app::AppHost> host;
    OverlayHost* overlays = nullptr;
};

void test_overlay_entry() {
    Fixture fixture;
    devtools::CompilerErrorOverlay overlay(*fixture.overlays);
    std::string error;
    const devtools::DiagnosticDocument document =
        devtools::parse_diagnostics(kErrorDocument, error);
    check(overlay.show(document).sequence != 0, "showing a failed compile returns a handle");
    check(fixture.overlays->size(OverlayLayer::Overlay) == 1,
          "the panel is an Overlay-layer entry, not a modal");
    fixture.overlays->flush();
    check(overlay.shown() == 1, "the panel mounts on the host's flush");
    Node* content = fixture.overlays->content(overlay.handle());
    check(content != nullptr, "the panel has a content root");
    check(content != nullptr && !content->children().empty(), "the panel renders its lines");
    check(fixture.overlays->layer_node(OverlayLayer::Modal) == nullptr,
          "no modal layer is created: the last good frame keeps running underneath");

    // A second failure replaces the document in place: one panel, not a pile.
    const devtools::DiagnosticDocument two = devtools::parse_diagnostics(kTwoErrorDocument, error);
    const auto before = overlay.handle();
    check(overlay.show(two).sequence == before.sequence, "a new document reuses the same panel");
    check(fixture.overlays->size(OverlayLayer::Overlay) == 1, "no second entry is opened");
    fixture.overlays->flush();
    check(overlay.document().diagnostics.size() == 2, "the panel now shows the new document");

    // The compile recovered: the panel goes away.
    check(overlay.show(devtools::parse_diagnostics(kCleanDocument, error)).sequence == 0,
          "a clean compile shows nothing");
    fixture.overlays->flush();
    check(fixture.overlays->size(OverlayLayer::Overlay) == 0, "the panel is closed");
    check(overlay.shown() == 0, "no entry is owned after recovery");
}

void test_warning_only_does_not_cover_the_app() {
    Fixture fixture;
    devtools::CompilerErrorOverlay overlay(*fixture.overlays);
    std::string error;
    check(overlay.show(devtools::parse_diagnostics(kWarningDocument, error)).sequence == 0,
          "a warnings-only document shows no panel");
    fixture.overlays->flush();
    check(fixture.overlays->size() == 0, "the app is not covered by a warning");
}

// The panel is a normal overlay entry, so it inherits the host's dismissal rules: Escape closes it
// because it is not persistent, which is what a developer expects from a temporary error panel.
void test_escape_dismisses() {
    Fixture fixture;
    devtools::CompilerErrorOverlay overlay(*fixture.overlays);
    std::string error;
    static_cast<void>(overlay.show(devtools::parse_diagnostics(kErrorDocument, error)));
    fixture.overlays->flush();
    check(fixture.overlays->handle_escape(), "Escape dismisses the error panel");
    fixture.overlays->flush();
    check(fixture.overlays->size(OverlayLayer::Overlay) == 0, "the panel is gone");
}

// The runtime-error seam: a boundary report reaches the same panel as a failed compile, and a
// cancellation shows nothing (a cancellation is a control outcome, not an error).
void test_boundary_sink() {
    Fixture fixture;
    devtools::CompilerErrorOverlay overlay(*fixture.overlays);
    devtools::BoundaryOverlaySink sink(overlay);

    tessera::ui::boundary::Report report;
    report.code = tessera::ui::boundary::BoundaryCode::Caught;
    report.boundary = 3;
    report.component = "comp::Counter";
    report.site = tessera::ui::boundary::FailureSite::Event;
    report.error.code = "user::invalid_state";
    report.error.message = "counter went negative";
    sink.on_report(report);
    fixture.overlays->flush();
    check(overlay.shown() == 1, "a runtime failure opens the same panel");
    const devtools::DiagnosticDocument& shown = overlay.document();
    check(shown.diagnostics.size() == 1, "one diagnostic for one report");
    if (!shown.diagnostics.empty()) {
        check(shown.diagnostics.front().message == "counter went negative",
              "the report's message is what the panel shows");
        check(shown.diagnostics.front().code.find("TESSERA_BOUNDARY_") == 0,
              "the boundary code is namespaced like a compiler code");
        check(shown.file == "comp::Counter", "the failing component is the document's subject");
    }

    // A cancellation must not cover the app.
    std::string error;
    static_cast<void>(overlay.show(devtools::parse_diagnostics(kCleanDocument, error)));
    fixture.overlays->flush();
    tessera::ui::boundary::Report cancellation;
    cancellation.code = tessera::ui::boundary::BoundaryCode::Cancelled;
    cancellation.error.kind = tessera::ui::boundary::ErrorClass::Cancellation;
    sink.on_report(cancellation);
    fixture.overlays->flush();
    check(overlay.shown() == 0, "a cancellation shows no panel");
}

} // namespace

int main() {
    test_overlay_entry();
    test_warning_only_does_not_cover_the_app();
    test_escape_dismisses();
    test_boundary_sink();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "compiler error overlay tests passed\n";
    return 0;
}
