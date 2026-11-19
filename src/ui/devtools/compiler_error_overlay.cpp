// SPDX-License-Identifier: MIT

#include "tessera/harness/json_parse.hpp"
#include "tessera/ui/devtools/compiler_error_overlay.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::devtools {
namespace {

// Rendered like the diagnostics a developer reads in a terminal: the code first so it can be
// searched, then the message on its own line, then the location.
std::string location(const OverlayDiagnostic& diagnostic) {
    if (!diagnostic.file || diagnostic.line == 0) {
        return {};
    }
    std::string text = *diagnostic.file + ":" + std::to_string(diagnostic.line);
    if (diagnostic.column != 0) {
        text += ":" + std::to_string(diagnostic.column);
    }
    return text;
}

const harness::JsonValue* member(const harness::JsonValue& value, std::string_view name) {
    return value.find(name);
}

std::string string_member(const harness::JsonValue& value, std::string_view name) {
    const harness::JsonValue* found = member(value, name);
    if (found == nullptr) {
        return {};
    }
    const std::optional<std::string_view> text = found->as_string();
    return text ? std::string(*text) : std::string();
}

unsigned integer_member(const harness::JsonValue& value, std::string_view name) {
    const harness::JsonValue* found = member(value, name);
    if (found == nullptr) {
        return 0;
    }
    const std::optional<std::int64_t> number = found->as_integer();
    if (!number || *number <= 0) {
        return 0;
    }
    // A line/column beyond what a source file can hold is not a location; clamp rather than wrap.
    constexpr std::int64_t kMaxLine = 1 << 24;
    return static_cast<unsigned>(std::min<std::int64_t>(*number, kMaxLine));
}

} // namespace

DiagnosticDocument parse_diagnostics(std::string_view text, std::string& error) {
    DiagnosticDocument document;
    error.clear();
    const auto parsed = harness::parse_json(text);
    if (!parsed) {
        error = "the diagnostics document is not valid JSON";
        return document;
    }
    const harness::JsonValue& root = *parsed;
    if (!root.is_object()) {
        error = "the diagnostics document is not a JSON object";
        return document;
    }
    const harness::JsonValue* version = member(root, "diagnosticsVersion");
    const std::optional<std::int64_t> version_number =
        version != nullptr ? version->as_integer() : std::nullopt;
    if (!version_number) {
        error = "the diagnostics document has no integer `diagnosticsVersion`";
        return document;
    }
    if (*version_number != 1) {
        // Spec section 6: reject an unknown version loudly instead of reinterpreting it.
        error = "unsupported diagnosticsVersion " + std::to_string(*version_number) +
                " (this build reads version 1)";
        return document;
    }
    const harness::JsonValue* list = member(root, "diagnostics");
    if (list == nullptr || !list->is_array()) {
        error = "the diagnostics document has no `diagnostics` array";
        return document;
    }
    document.version = 1;
    document.file = string_member(root, "file");
    for (const harness::JsonValue& entry : *list->as_array()) {
        if (!entry.is_object()) {
            continue;
        }
        OverlayDiagnostic diagnostic;
        diagnostic.code = string_member(entry, "code");
        diagnostic.message = string_member(entry, "message");
        const std::string severity = string_member(entry, "severity");
        diagnostic.severity = severity.empty() ? std::string("error") : severity;
        diagnostic.suggestion = string_member(entry, "suggestion");
        if (const harness::JsonValue* span = member(entry, "span");
            span != nullptr && span->is_object()) {
            diagnostic.file = string_member(*span, "file");
            diagnostic.line = integer_member(*span, "line");
            diagnostic.column = integer_member(*span, "column");
            if (diagnostic.file->empty() || diagnostic.line == 0) {
                diagnostic.file.reset();
                diagnostic.line = 0;
                diagnostic.column = 0;
            }
        }
        document.diagnostics.push_back(std::move(diagnostic));
    }
    return document;
}

bool has_errors(const DiagnosticDocument& document) {
    return std::any_of(
        document.diagnostics.begin(), document.diagnostics.end(),
        [](const OverlayDiagnostic& diagnostic) { return diagnostic.severity != "warning"; });
}

std::vector<std::string> overlay_lines(const DiagnosticDocument& document,
                                       std::size_t max_diagnostics) {
    std::vector<std::string> lines;
    if (document.diagnostics.empty()) {
        return lines;
    }
    const std::string header =
        document.file.empty() ? std::string("compile failed") : "compile failed: " + document.file;
    lines.push_back(header);
    std::size_t shown = 0;
    for (const OverlayDiagnostic& diagnostic : document.diagnostics) {
        if (shown == max_diagnostics) {
            lines.push_back("(" + std::to_string(document.diagnostics.size() - shown) + " more)");
            break;
        }
        lines.push_back(diagnostic.severity + " " + diagnostic.code);
        lines.push_back(diagnostic.message);
        const std::string where = location(diagnostic);
        if (!where.empty()) {
            lines.push_back(where);
        }
        if (!diagnostic.suggestion.empty()) {
            lines.push_back("hint: " + diagnostic.suggestion);
        }
        ++shown;
    }
    return lines;
}

CompilerErrorOverlay::CompilerErrorOverlay(OverlayHost& host)
    : host_(host) {}

void CompilerErrorOverlay::mount(Node& content) const {
    // The panel is a plain scene subtree built from the document's lines: one Text node per line,
    // so the existing style cascade, wrapping and accessibility tree apply. The overlay host
    // already gives this content root its role, layer and dismissal policy.
    const std::vector<std::string> lines = overlay_lines(document_);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        // Ids come from the same private range the host uses, offset past the entries it could ever
        // create, so a panel node never collides with a generated or virtual one.
        const NodeId id = OverlayHost::kFirstNodeId + ((NodeId{1} << 40) | NodeId{index});
        Node& text = content.add_child(std::make_unique<Node>(id));
        text.set_element_kind("Text");
        text.set_class_name("tessera-compiler-error-line");
        PaintData paint;
        paint.text = lines[index];
        text.set_paint_data(std::move(paint));
    }
}

OverlayHandle CompilerErrorOverlay::show(const DiagnosticDocument& document) {
    document_ = document;
    if (!has_errors(document_)) {
        // A warnings-only or clean document is a recovered compile: the panel must go away rather
        // than keep reporting a failure that no longer exists.
        clear();
        return {};
    }
    if (handle_) {
        // One panel per overlay: replace the document it shows instead of stacking a second entry.
        Node* content = host_.content(handle_);
        if (content != nullptr) {
            // remove_child erases from children(), so a range-for would iterate a mutating vector
            // and dereference moved-from (null) entries.
            while (!content->children().empty()) {
                static_cast<void>(content->remove_child(*content->children().front()));
            }
            mount(*content);
            return handle_;
        }
        handle_ = {};
    }
    OverlaySpec spec;
    spec.id = std::string(kElementId);
    spec.layer = kLayer;
    // The app keeps running; the panel sits above it until the compile succeeds or the developer
    // dismisses it. Persistent because a blur must not hide the reason the build failed.
    spec.persistent = true;
    spec.mount = [this](Node& content) { mount(content); };
    handle_ = host_.open(std::move(spec));
    return handle_;
}

void CompilerErrorOverlay::clear() {
    if (!handle_) {
        return;
    }
    static_cast<void>(host_.close(handle_));
    handle_ = {};
}

std::size_t CompilerErrorOverlay::shown() const {
    return handle_ && host_.is_open(handle_) ? 1 : 0;
}

BoundaryOverlaySink::BoundaryOverlaySink(CompilerErrorOverlay& overlay)
    : overlay_(overlay) {}

void BoundaryOverlaySink::on_report(const boundary::Report& report) {
    if (report.code == boundary::BoundaryCode::Cancelled) {
        return;
    }
    // One diagnostic in the document's own shape, so the panel does not need a second rendering
    // path: the boundary report becomes a `compiler-diagnostics-v1` diagnostic with the component
    // as its "file" and the failure site as the code.
    DiagnosticDocument document;
    document.version = 1;
    document.file = report.component;
    OverlayDiagnostic diagnostic;
    diagnostic.code = "TESSERA_BOUNDARY_" + std::string(boundary::code_name(report.code));
    diagnostic.severity = "error";
    diagnostic.message = report.error.message.empty()
                             ? std::string("a component failed without a message")
                             : report.error.message;
    diagnostic.suggestion = "site: " + std::string(boundary::site_name(report.site));
    document.diagnostics.push_back(std::move(diagnostic));
    static_cast<void>(overlay_.show(document));
}

} // namespace tessera::ui::devtools
