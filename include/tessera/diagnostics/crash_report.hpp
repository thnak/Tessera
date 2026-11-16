// SPDX-License-Identifier: MIT

// Reading back the record a crash wrote, and turning it into the redacted payload the uploader may
// send (docs/specs/crash-reporting-v1.md sections 4-6.1).
//
// The handler writes a flat `key=value` record with module-relative frames; this header parses it,
// redacts it, and produces `CrashMetadata` (the shape `crash.hpp` already owns). Parsing and
// redaction run *after* the crash, in a healthy process: they may allocate, and they do, because
// that is when it is safe.
//
// Invariants that matter for privacy (section 5): a parsed report never carries an absolute user
// path, an environment value, or a raw module path. The record stores module *names* only, the
// parser re-applies `redact_path`, and `redact_report` is the second line of defence that drops a
// line whose value still looks absolute.

#pragma once

#include "tessera/diagnostics/crash.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::diagnostics {

// One module from the record's module table. `name` is a basename, never a path.
struct CrashModule {
    std::string name;
    std::uint64_t base = 0;
    std::uint64_t size = 0;
};

// A frame as the record stores it: module index plus offset, no symbol (symbols need the CI symbol
// files, which the handler cannot read).
struct CrashRawFrame {
    std::size_t module = 0;
    std::uint64_t offset = 0;
};

struct CrashRecord {
    bool valid = false;
    // True when the record was cut short (a line did not fit): a truncated report says so instead
    // of looking complete.
    bool truncated = false;
    std::string reason;
    std::uint64_t code = 0;
    std::string thread;
    std::uint64_t address = 0;
    std::string app_version;
    std::string commit;
    std::string build_id;
    std::string platform;
    std::string arch;
    std::string os;
    std::string gpu;
    std::string config_hash;
    std::vector<CrashModule> modules;
    std::vector<CrashRawFrame> frames;
    // Frames whose program counter matched no known module (a JIT frame, an unloaded module).
    std::uint64_t unmatched_frames = 0;
};

enum class CrashParseError {
    NotACrashRecord, // empty, or the first line is not a schema this build knows
    Unsupported,     // a `tessera.crash.` schema other than v1
    Malformed,       // a line that is neither a known key nor `module=`/`frame=`
};

[[nodiscard]] std::string_view to_string(CrashParseError error) noexcept;

// Parses a record. Never throws; a malformed line makes the whole record invalid rather than
// silently producing a report with missing fields.
[[nodiscard]] std::expected<CrashRecord, CrashParseError> parse_crash_record(std::string_view text);

// Maps the record onto the metadata model of `crash.hpp`, resolving each frame's module index to a
// redacted module name and leaving `symbol` empty (symbolication is a separate step).
[[nodiscard]] CrashMetadata crash_record_metadata(const CrashRecord& record);

// The record's frames as `CrashFrame`s with their module names and no symbols. The caller owns the
// storage: `CrashMetadata::frames` is a span, so it must point at something that outlives it. This
// is what a caller that has no symbol file (the uploader's grouping key) uses.
[[nodiscard]] std::vector<CrashFrame> crash_record_frames(const CrashRecord& record);

// The privacy pass of section 5, applied to a whole text: absolute user-home prefixes become
// `<home>` (`redact_paths` from `log.hpp`), a `C:\...` or `/...` value line whose key is path-like
// is reduced to its last component, and an environment-assignment shape (`NAME=value` where NAME is
// upper-case) is dropped entirely. Idempotent, so it can run before and after a transformation.
[[nodiscard]] std::string redact_report(std::string_view text);

// True when `text` still contains something the redaction rules forbid: an absolute user path, a
// path-like value with a separator, or an environment assignment. The uploader calls this before it
// sends; the tests call it to prove a marker is gone.
[[nodiscard]] bool contains_unredacted(std::string_view text);

} // namespace tessera::diagnostics
