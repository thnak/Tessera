// SPDX-License-Identifier: MIT

#pragma once

#include "project_identity.hpp"
#include "regenerate.hpp"
#include "regenerate_source_file.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

// The stylesheets one codegen run embeds (issue #80) and the text each blob compiles (issue #463).
// Split out of the driver: resolving an import, reading the file and deciding what the blob
// contains is one concern, and `regenerate.cpp` is already over the hard file size limit.
namespace tessera::compiler::codegen::cli::internal {

// One stylesheet of the run, in the two forms the pipeline needs. They differ only for a CSS
// module: the blob compiles the scoped rewrite while the lowering hashes the file's own bytes,
// because the hash identifies the source file and must not change when only the scoping changes.
struct StylesheetSource {
    std::string path;      // project-relative POSIX path; identifies the blob and the StylesheetRef
    std::string file_text; // the bytes on disk; `ir::StylesheetRef::hash` is over these
    std::string blob_text; // what the blob compiles: the scoped rewrite, else the file bytes
};

// Reads every stylesheet the sources import, one entry per path in first-import order. A plain
// `import "./x.css"` and a `import styles from "./x.module.css"` both count, and a file imported
// both ways is one entry (`blob_text` depends only on the path, so the order cannot change it).
// A missing file is not offered, so the lowering reports it at the import's span
// (`TESSERA_IR_STYLESHEET_MISSING`); a file that exists but cannot be read, and a CSS module that
// does not scan, are build failures reported here. Returns false after reporting, exit code set.
[[nodiscard]] bool collect_stylesheets(std::span<const std::unique_ptr<detail::SourceFile>> sources,
                                       const Options& options, const ProjectIdentity& identity,
                                       bool project_paths, std::vector<StylesheetSource>& sheets,
                                       Outcome& outcome);

} // namespace tessera::compiler::codegen::cli::internal
