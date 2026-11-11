// SPDX-License-Identifier: MIT

// Crash-report metadata: the bounded, redacted payload the crash handler writes and the next launch
// reads back (docs/specs/crash-reporting-v1.md sections 4-6.1). This header owns the *report shape
// and its canonical JSON*, not the platform capture: a handler fills the fields it can capture
// asynchronously and calls `crash_metadata_json`.
//
// Determinism: no wall clock, no addresses that depend on ASLR, no heap ordering. Frames carry a
// module-relative offset and breadcrumbs carry a caller-assigned sequence, so two runs of the same
// crash produce byte-identical metadata (and the golden tests are stable).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tessera::diagnostics {

// One stack frame. `module` is a basename, never a full path (see `redact_path`).
struct CrashFrame {
    std::string_view module;
    std::string_view symbol; // empty when symbols are missing: reported, never silently dropped
    std::uint64_t offset = 0;
    std::uint32_t line = 0;
};

// One bounded breadcrumb: what happened before the crash. `message` must already be free of user
// data; this writer caps its length but does not guess what is private.
struct CrashBreadcrumb {
    std::string_view category;
    std::string_view message;
    std::uint64_t sequence = 0;
};

// The metadata a handler can capture without allocating in the fatal path. Every field is a view:
// the caller owns the storage and fills what it has; an empty field is omitted from the JSON.
struct CrashMetadata {
    std::string_view app_version;
    std::string_view commit;
    std::string_view build_id;
    std::string_view platform;
    std::string_view arch;
    std::string_view os;
    std::string_view gpu;
    std::string_view config_hash;
    std::string_view crash_reason; // "access_violation", "abort", "device_lost", ...
    std::string_view thread;       // "main", "render", ...
    std::string_view last_trace_id;
    std::span<const CrashFrame> frames;           // index 0 = innermost frame
    std::span<const CrashBreadcrumb> breadcrumbs; // oldest first
};

struct CrashReportLimits {
    std::size_t max_frames = 64;
    std::size_t max_breadcrumbs = 128;
    // Bounds the frames and breadcrumbs arrays; the fixed envelope (schema, signature, counters)
    // may add up to 1 KiB, so a capped document stays within this plus that reserve.
    std::size_t max_text_bytes = 64U * 1024U;
    std::size_t max_field_bytes = 256;      // per short metadata string
    std::size_t max_breadcrumb_bytes = 160; // per breadcrumb message
    std::size_t max_signature_depth = 3;    // frames that make up the grouping signature
};

// Canonical JSON of the metadata. Input strings are escaped to ASCII (bytes >= 0x7F become \u00XX,
// so a truncated multi-byte character cannot corrupt the document) and capped; `frames` and
// `breadcrumbs` are capped from the innermost/newest end and every applied cap is reported in
// `truncated` together with what was dropped. The result is always a valid JSON object.
[[nodiscard]] std::string crash_metadata_json(const CrashMetadata& metadata,
                                              CrashReportLimits limits = {});

// The grouping key of section 4 ("top frames/module/build"): build id plus the innermost
// `max_signature_depth` frames as `module!symbol+offset`, joined with '|'. Symbol names and module
// basenames only - no paths, addresses or user data. Stable for the same crash and build.
[[nodiscard]] std::string crash_signature(const CrashMetadata& metadata,
                                          CrashReportLimits limits = {});

// Redacts a path-ish string to its last component: keeps `main.cpp` from
// `C:\Users\someone\app\main.cpp` or `/home/someone/app/main.cpp`, and returns the input unchanged
// when it has no separator. This is the rule for `CrashFrame::module` and for breadcrumb paths.
[[nodiscard]] std::string redact_path(std::string_view path);

} // namespace tessera::diagnostics
