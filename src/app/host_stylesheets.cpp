// SPDX-License-Identifier: MIT
//
// Embedded stylesheet registration (ADR-048). Split out of host.cpp, which is at its size budget.
//
// Registration is a startup-time operation: every blob is loaded here, once, and resolved into the
// cascade; no frame ever parses CSS. The constructor sheet is kept as the base of every rebuild, so
// a later call replaces the previous registration rather than stacking.

#include "host_impl.hpp"

#include <cstdio>
#include <string_view>
#include <utility>

namespace tessera::app {

namespace {

const char* blob_error_name(ui::style::BlobErrorCode code) {
    switch (code) {
    case ui::style::BlobErrorCode::Truncated:
        return "Truncated";
    case ui::style::BlobErrorCode::BadMagic:
        return "BadMagic";
    case ui::style::BlobErrorCode::UnsupportedFormatVersion:
        return "UnsupportedFormatVersion";
    case ui::style::BlobErrorCode::EngineSchemaMismatch:
        return "EngineSchemaMismatch";
    case ui::style::BlobErrorCode::HashMismatch:
        return "HashMismatch";
    case ui::style::BlobErrorCode::Corrupt:
        return "Corrupt";
    }
    return "Corrupt";
}

// A skipped embedded sheet is reported, never silently swallowed (ADR-048: an embedded blob is
// build output, so a load failure is a build/runtime version mismatch, not a CSS error).
void report_blob_failure(std::string_view path, const ui::style::BlobError& error) {
    std::fprintf(stderr, "TESSERA_CSS_BLOB_INVALID: %.*s: %s: %s\n", static_cast<int>(path.size()),
                 path.data(), blob_error_name(error.code), error.message.c_str());
}

} // namespace

void AppHost::Impl::register_embedded_stylesheets(
    std::span<const ui::style::EmbeddedStylesheet> sheets) {
    embedded_sheets.clear();
    rejected_stylesheet_count = 0;
    for (const ui::style::EmbeddedStylesheet& embedded : sheets) {
        auto prepared = ui::style::load_stylesheet_blob(embedded.bytes);
        if (!prepared) {
            report_blob_failure(embedded.path, prepared.error());
            ++rejected_stylesheet_count;
            continue;
        }
        embedded_sheets.push_back(std::move(*prepared));
    }
    // The provider borrows the active sheet and the tree captures the scene, so both are rebuilt
    // now (a direct paint before the next begin_frame() stays consistent). The style generation
    // changed: mark every node so the next begin_frame() re-resolves and rebuilds the selector
    // tree, and drop the per-node paint cache because a node can stay Clean while its computed
    // style changes (inheritance, ancestor/sibling selectors, a new sheet).
    apply_style_generation(ui::Dirty::Style | ui::Dirty::Layout | ui::Dirty::Paint |
                           ui::Dirty::Render);
}

} // namespace tessera::app
