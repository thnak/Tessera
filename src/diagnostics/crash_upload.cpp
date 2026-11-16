// SPDX-License-Identifier: MIT

// The opt-in uploader. Every gate is checked in the order the header documents, and the transport
// is not reached until all of them pass - which is what makes "disabled sends nothing" a structural
// property rather than a promise.

#include "tessera/diagnostics/crash.hpp"
#include "tessera/diagnostics/crash_report.hpp"
#include "tessera/diagnostics/crash_upload.hpp"

#include <cstddef>
#include <vector>

namespace tessera::diagnostics {

bool MemoryTransport::send(const CrashUploadPayload& payload) {
    calls_.push_back(payload);
    return accept_;
}

std::string_view to_string(CrashUploadStatus status) noexcept {
    switch (status) {
    case CrashUploadStatus::Disabled:
        return "disabled";
    case CrashUploadStatus::NoEndpoint:
        return "no endpoint";
    case CrashUploadStatus::RedactionFailed:
        return "redaction failed";
    case CrashUploadStatus::Sent:
        return "sent";
    case CrashUploadStatus::TransportFailed:
        return "transport failed";
    }
    return "disabled";
}

bool upload_enabled(const CrashUploadConfig& config) noexcept {
    return config.enabled && config.consent && !config.endpoint.empty();
}

CrashUploadStatus upload_crash_report(const CrashUploadConfig& config, std::string_view report_text,
                                      CrashUploadTransport& transport) {
    // Gate 1: opt-in. Returning here is the proof that a default config never sends: the transport
    // is not even referenced below.
    if (!config.enabled || !config.consent) {
        return CrashUploadStatus::Disabled;
    }
    // Gate 2: an explicit endpoint, never a built-in default.
    if (config.endpoint.empty()) {
        return CrashUploadStatus::NoEndpoint;
    }
    if (report_text.empty()) {
        return CrashUploadStatus::Disabled;
    }

    // Gate 3: redact, then verify. Redaction runs first so the grouping signature is derived from
    // the text that will actually be transmitted - parsing the raw record would let a dropped
    // environment line fail the parse and silently strip the signature.
    const std::string redacted = redact_report(report_text);
    if (contains_unredacted(redacted)) {
        return CrashUploadStatus::RedactionFailed;
    }
    const auto parsed = parse_crash_record(redacted);

    CrashUploadPayload payload;
    payload.endpoint.assign(config.endpoint);
    payload.body = redacted;
    if (parsed.has_value()) {
        // The frames must outlive the metadata that points at them; both live in this scope.
        const std::vector<CrashFrame> frames = crash_record_frames(*parsed);
        CrashMetadata metadata = crash_record_metadata(*parsed);
        metadata.frames = frames;
        payload.signature = crash_signature(metadata);
    }
    return transport.send(payload) ? CrashUploadStatus::Sent : CrashUploadStatus::TransportFailed;
}

} // namespace tessera::diagnostics
