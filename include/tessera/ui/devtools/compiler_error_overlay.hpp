// SPDX-License-Identifier: MIT

#pragma once

// The dev-mode compiler-error overlay (issue #78 item 4): a failed compile keeps the last good
// frame and shows the diagnostic with its TSX location instead of leaving a blank window.
//
// It is a *consumer*, not a second diagnostic path. The document it renders is the one
// docs/specs/compiler-diagnostics-v1.md defines and `tools/codegen/diagnostics_json.cpp` writes, so
// the overlay and the preview page and CI all read the same bytes. Nothing here re-derives a
// diagnostic from compiler text.
//
// Two seams, deliberately separate from the rendering:
//   * `DiagnosticDocument` — the parsed document, owned as plain data so it can be tested without a
//     scene, a window or the codegen CLI;
//   * `CompilerErrorOverlay` — the overlay entry: it mounts into the host's existing `OverlayHost`
//     (issue #302), so it inherits the layer stacking, dismissal, focus and hit-testing rules
//     instead of opening a window of its own.
//
// Dev-mode only. The overlay is a normal overlay-host entry, so a release build that never opens
// one gains no nodes, and the parse is only reached by whoever reads a diagnostics document.

#include "tessera/harness/json_value.hpp"
#include "tessera/ui/boundary/report.hpp"
#include "tessera/ui/overlay/overlay.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::devtools {

// One diagnostic of the document, in the shape docs/specs/compiler-diagnostics-v1.md section 3
// defines. `span` is absent for a setup/usage diagnostic, which the overlay must render without a
// location rather than at a guessed position (section 3).
struct OverlayDiagnostic {
    std::string code;
    std::string severity = "error";
    std::string message;
    std::string suggestion;
    // Present only when the document carried a `span` with a file and a line.
    std::optional<std::string> file;
    unsigned line = 0;
    unsigned column = 0;
};

// One parsed `compiler-diagnostics-v1` document.
struct DiagnosticDocument {
    // The document's `diagnosticsVersion`; 0 when the text was not a document. A consumer rejects
    // an unknown version loudly instead of reinterpreting it (spec section 6).
    int version = 0;
    std::string file; // the run's primary input, "" when the run had none
    std::vector<OverlayDiagnostic> diagnostics;
};

// Parses one document. `error` is set and the result has `version == 0` when `text` is not a
// diagnostics document of a version this build understands; a document with an empty
// `diagnostics` array is a *success*, not a failure (section 2), and parses to an empty list.
[[nodiscard]] DiagnosticDocument parse_diagnostics(std::string_view text, std::string& error);

// True when the document holds at least one error-severity diagnostic: the overlay is for a failed
// compile, and a warnings-only document must not cover the app.
[[nodiscard]] bool has_errors(const DiagnosticDocument& document);

// The lines the overlay renders, in display order: a header naming the primary file, then one block
// per diagnostic (`severity code`, the message, `file:line:column`, and the suggestion when there
// is one). Bounded: at most `max_diagnostics` blocks, so a pathological document cannot grow the
// overlay without limit.
[[nodiscard]] std::vector<std::string> overlay_lines(const DiagnosticDocument& document,
                                                     std::size_t max_diagnostics = 8);

// The overlay entry for a failed compile.
//
// `open()` records the request on the host and returns the handle; the content mounts on the host's
// next flush (its documented frame-boundary rule), so a caller may open from anywhere on the UI
// thread. Re-opening replaces the shown document instead of stacking a second overlay: a developer
// editing a file that keeps failing sees one error panel that updates, not a pile of them.
class CompilerErrorOverlay {
public:
    // Layer and id of the panel: an `Overlay`-layer entry, never a modal, so the last good frame
    // stays visible and the app keeps running underneath it.
    static constexpr OverlayLayer kLayer = OverlayLayer::Overlay;
    static constexpr std::string_view kElementId = "tessera-compiler-error";

    explicit CompilerErrorOverlay(OverlayHost& host);

    CompilerErrorOverlay(const CompilerErrorOverlay&) = delete;
    CompilerErrorOverlay& operator=(const CompilerErrorOverlay&) = delete;

    // Shows `document`. A document with no error-severity diagnostic closes any open panel and
    // returns an empty handle: the compile recovered, so the overlay goes away. Returns the panel's
    // handle, or empty when nothing is shown.
    OverlayHandle show(const DiagnosticDocument& document);

    // Closes the panel if it is open.
    void clear();

    [[nodiscard]] const DiagnosticDocument& document() const {
        return document_;
    }
    // Live or pending entries this object owns; 0 after clear().
    [[nodiscard]] std::size_t shown() const;
    [[nodiscard]] OverlayHandle handle() const {
        return handle_;
    }

private:
    // Builds the panel content into an overlay entry's content root.
    void mount(Node& content) const;

    OverlayHost& host_;
    DiagnosticDocument document_;
    OverlayHandle handle_;
};

// The runtime-error side of the same panel (docs/specs/error-boundaries-v1.md section 6): a
// `ReportSink` the boundary table delivers to, so a component failure reaches the same overlay a
// failed compile does instead of a second, separately-styled window.
//
// The boundary report is already redacted and bounded (`make_report`), so this adapter only formats
// it: the overlay never sees a raw path or unbounded text.
class BoundaryOverlaySink final : public boundary::ReportSink {
public:
    explicit BoundaryOverlaySink(CompilerErrorOverlay& overlay);

    // A cancellation is a control outcome, not an error (spec section 1), so it shows nothing.
    // Every other report replaces the panel's document, which means a runtime failure and a compile
    // failure are one panel: whichever happened last is what the developer sees.
    void on_report(const boundary::Report& report) override;

private:
    CompilerErrorOverlay& overlay_;
};

} // namespace tessera::ui::devtools
