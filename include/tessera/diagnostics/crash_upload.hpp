// SPDX-License-Identifier: MIT

// Opt-in crash-report uploader (docs/specs/crash-reporting-v1.md sections 4-6.1).
//
// The contract this header exists to enforce, in order:
//
// 1. **Off by default.** `CrashUploadConfig::enabled` is false and every other gate defaults
//    closed: no endpoint, no consent. `upload_crash_report` returns `Disabled` before it looks at
//    the report, so "off" costs one branch and cannot reach a transport.
// 2. **Consent before content.** `enabled` alone is not enough: `consent` must also be true, so a
//    config that turns the uploader on but never asked the user still sends nothing.
// 3. **Redaction before transmission.** The payload is the redacted text (`redact_report`), and
//    `contains_unredacted` is checked as a last gate: a report that still carries a user path is
//    refused (`RedactionFailed`) rather than uploaded.
// 4. **No transport in tests.** The transport is an interface; the only implementation here is
//    `MemoryTransport`, which records instead of sending. Production wiring (TLS/auth) is a later
//    slice and a dependency decision (`docs/specs/dependencies.md`, section 8).

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::diagnostics {

// What the uploader hands a transport. `body` is already redacted.
struct CrashUploadPayload {
    std::string endpoint;
    std::string body;
    std::string signature; // grouping key, never a path or an address
};

// The seam a real HTTP client (libcurl, when its dependency decision lands) implements. The
// uploader never blocks on it from a UI thread: it is called from the upload worker, and the caller
// decides when to drain.
class CrashUploadTransport {
public:
    virtual ~CrashUploadTransport() = default;

    // Returns true when the transport accepted the payload. A false result is retried later, not
    // treated as a crash.
    virtual bool send(const CrashUploadPayload& payload) = 0;
};

// Records every call instead of sending. Used by the tests to prove that nothing is sent when the
// uploader is disabled, and as the default transport when a caller has not wired one.
class MemoryTransport final : public CrashUploadTransport {
public:
    bool send(const CrashUploadPayload& payload) override;

    [[nodiscard]] const std::vector<CrashUploadPayload>& calls() const noexcept {
        return calls_;
    }
    [[nodiscard]] std::size_t call_count() const noexcept {
        return calls_.size();
    }
    void set_accept(bool accept) noexcept {
        accept_ = accept;
    }

private:
    std::vector<CrashUploadPayload> calls_;
    bool accept_ = true;
};

struct CrashUploadConfig {
    // Master switch. False unless a caller explicitly opts in.
    bool enabled = false;
    // The user's answer to "send crash reports?". Both must be true.
    bool consent = false;
    // Explicit endpoint: no default, so a build cannot upload by accident.
    std::string_view endpoint;
    // Sent along for grouping/version matching; never a path.
    std::string_view build_id;
    // Bound on retries per report. 0 keeps the report queued but never retried, which is the
    // documented "bounded retries/backoff" floor.
    std::size_t max_attempts = 3;
};

enum class CrashUploadStatus {
    Disabled,        // not enabled, or no consent: nothing was read, nothing was sent
    NoEndpoint,      // enabled with consent but no endpoint: refuse rather than guess
    RedactionFailed, // the report still carried user data: refuse to transmit
    Sent,            // the transport accepted it
    TransportFailed, // the transport refused it; keep the report for a later attempt
};

[[nodiscard]] std::string_view to_string(CrashUploadStatus status) noexcept;

// True only when every gate is open. Tests assert this is false for a default config.
[[nodiscard]] bool upload_enabled(const CrashUploadConfig& config) noexcept;

// Uploads one already-parsed report. `report_text` must be the raw record; the uploader redacts it
// itself. Does nothing (and touches no transport) unless `upload_enabled` holds.
[[nodiscard]] CrashUploadStatus upload_crash_report(const CrashUploadConfig& config,
                                                    std::string_view report_text,
                                                    CrashUploadTransport& transport);

} // namespace tessera::diagnostics
