// SPDX-License-Identifier: MIT

// `FrameDiagnostics` and its three counter subjects are header-only (include/tessera/runtime/
// frame_counters.hpp): they are small, hot, and every method is a field update or an accessor, so
// inlining them is what a frame-path caller wants and there is no state to hide behind a TU
// boundary (docs/architecture/god-type-audit-v1.md section 3.9).
//
// This TU stays in the build so the target keeps one source file per former responsibility and so
// a future non-inline part has an obvious home; it intentionally defines nothing.
