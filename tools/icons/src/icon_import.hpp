// SPDX-License-Identifier: MIT
//
// `tessera icons add <set>:<name>` -- the Iconify import (ADR-047 item 8).
//
// The import turns referenced icons from pinned, on-disk Iconify sets into embedded vector assets:
//
//   <out>/icons/<prefix>/<name>.svg   the vector asset (the PaintKind::Vector payload)
//   <out>/icons.manifest              ready for tessera_embed_assets(MANIFEST ...)
//   <out>/icons.g.hpp                 generated accessors: name -> asset, size, monochrome
//   <out>/ICON_LICENSES.txt           attribution for every imported set
//
// Tree-shaking is inherent: only the icons named by the reference list are emitted, whatever else
// the pinned set contains. Every byte is a pure function of the references and the pinned set, so
// two runs produce identical output (the golden test asserts exactly that).

#pragma once

#include "icon_types.hpp"

#include <expected>
#include <string>
#include <vector>

namespace tessera::icons {

struct ImportRequest {
    // Directory of pinned sets: <root>/<prefix>/{info.json,icons.json}.
    std::string iconify_root;
    // `prefix:name` references. Order and duplicates do not matter; the result is sorted.
    std::vector<std::string> references;
    // Explicit opt-in for share-alike sets (ADR-047 item 8).
    bool allow_share_alike = false;
    // Force every paint to `currentColor` so the icon follows the text color token.
    bool monochrome = false;
};

// Runs the whole import. Fails with one message naming the set, the license and the reason when a
// license gate rejects a set, and with a message naming the reference when an icon is missing.
[[nodiscard]] std::expected<ImportResult, std::string> import_icons(const ImportRequest& request);

// Writes the emitted files under `root`, creating directories.
[[nodiscard]] std::expected<void, std::string> write_files(const std::string& root,
                                                           const std::vector<EmittedFile>& files);

// Compares the emitted bytes with the files under `root` (missing file, stale file or a differing
// byte is a failure). Used by --check and by the golden test.
[[nodiscard]] std::expected<void, std::string> check_files(const std::string& root,
                                                           const std::vector<EmittedFile>& files);

} // namespace tessera::icons
