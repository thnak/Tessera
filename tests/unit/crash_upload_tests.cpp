// SPDX-License-Identifier: MIT

// Uploader opt-in and redaction (issue #192: "the uploader is opt-in and redaction tests pass";
// docs/specs/crash-reporting-v1.md sections 4-5).
//
// Two proofs, both structural rather than documentary:
//   1. **Zero upload when disabled.** A `MemoryTransport` counts every call. A default config, a
//      config with `enabled` but no consent, and a config with consent but no endpoint all assert
//      `call_count() == 0`.
//   2. **Redaction with a planted marker.** A report is built with a username-like absolute path
//      (`C:\Users\planted-marker-9f3\...` and `/home/planted-marker-9f3/...`) and an environment
//      value; the test asserts the marker is absent from the redacted body, that
//      `contains_unredacted` agrees, and that the uploaded payload - if anything is uploaded at
//      all - does not carry it.

#include "tessera/diagnostics/crash_report.hpp"
#include "tessera/diagnostics/crash_upload.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// The marker: a username-like component that must never survive redaction.
constexpr std::string_view kMarker = "planted-marker-9f3";

using tessera::diagnostics::CrashUploadConfig;
using tessera::diagnostics::CrashUploadStatus;
using tessera::diagnostics::MemoryTransport;
using tessera::diagnostics::upload_crash_report;
using tessera::diagnostics::upload_enabled;

std::string report_with_marker() {
    std::string text = "tessera.crash.v1\n"
                       "reason=access_violation\n"
                       "app=1.0.0\n"
                       "build=bld-redact\n";
    // A module name that is an absolute path with the planted username: the writer stores
    // basenames, but a hand-edited or older record must not be able to smuggle one through.
    text += "module=";
    text += "C:\\Users\\";
    text += kMarker;
    text += "\\app\\app.exe|0x140000000|0x1000\n";
    text += "frame=0|0x20\n";
    // An environment assignment: never part of a report, dropped whole.
    text += "HOME=C:\\Users\\";
    text += kMarker;
    text += "\n";
    text += "PATH=/usr/local/bin:/usr/bin\n";
    return text;
}

void test_zero_upload_when_disabled() {
    const std::string report = report_with_marker();

    // (a) The default config: every gate closed.
    {
        MemoryTransport transport;
        const CrashUploadConfig config;
        check(!upload_enabled(config), "the default config is not enabled");
        const auto status = upload_crash_report(config, report, transport);
        check(status == CrashUploadStatus::Disabled, "a default config is Disabled");
        check(transport.call_count() == 0, "a default config sends nothing");
    }
    // (b) enabled but no consent: the user never said yes.
    {
        MemoryTransport transport;
        CrashUploadConfig config;
        config.enabled = true;
        config.endpoint = "https://crash.example.invalid/v1";
        check(!upload_enabled(config), "enabled without consent is still off");
        const auto status = upload_crash_report(config, report, transport);
        check(status == CrashUploadStatus::Disabled, "enabled without consent is Disabled");
        check(transport.call_count() == 0, "enabled without consent sends nothing");
    }
    // (c) consent but no endpoint: nothing to send to.
    {
        MemoryTransport transport;
        CrashUploadConfig config;
        config.enabled = true;
        config.consent = true;
        check(!upload_enabled(config), "consent without an endpoint is still off");
        const auto status = upload_crash_report(config, report, transport);
        check(status == CrashUploadStatus::NoEndpoint, "consent without an endpoint is NoEndpoint");
        check(transport.call_count() == 0, "consent without an endpoint sends nothing");
    }
    // (d) An empty report with every gate open still sends nothing.
    {
        MemoryTransport transport;
        CrashUploadConfig config;
        config.enabled = true;
        config.consent = true;
        config.endpoint = "https://crash.example.invalid/v1";
        check(upload_enabled(config), "every gate open is enabled");
        const auto status = upload_crash_report(config, "", transport);
        check(status == CrashUploadStatus::Disabled, "an empty report is not uploaded");
        check(transport.call_count() == 0, "an empty report sends nothing");
    }
}

void test_redaction_with_planted_marker() {
    const std::string report = report_with_marker();
    check(report.find(kMarker) != std::string::npos, "the fixture really contains the marker");
    check(tessera::diagnostics::contains_unredacted(report),
          "the raw report is detected as unredacted");

    const std::string redacted = tessera::diagnostics::redact_report(report);
    std::cerr << "----- redacted begin -----\n" << redacted << "----- redacted end -----\n";
    check(redacted.find(kMarker) == std::string::npos,
          "the planted marker is absent after redaction");
    check(redacted.find("<home>") != std::string::npos ||
              redacted.find("app.exe") != std::string::npos,
          "the path is reduced rather than mangled away entirely");
    check(redacted.find("app.exe") != std::string::npos, "the module basename survives redaction");
    check(redacted.find("# env-dropped") != std::string::npos,
          "an environment assignment is dropped and marked");
    check(!tessera::diagnostics::contains_unredacted(redacted),
          "the redacted text passes the unredacted check");
    check(tessera::diagnostics::redact_report(redacted) == redacted, "redaction is idempotent");

    // The same rules through the metadata model: a frame's module is a basename, never a path.
    tessera::diagnostics::CrashRecord record;
    record.valid = true;
    record.build_id = "bld-redact";
    record.modules.push_back(tessera::diagnostics::CrashModule{
        tessera::diagnostics::redact_path("C:\\Users\\" + std::string(kMarker) + "\\app\\app.exe"),
        0x140000000ULL, 0x1000});
    record.frames.push_back(tessera::diagnostics::CrashRawFrame{0, 0x20});
    const auto parsed = tessera::diagnostics::parse_crash_record(report);
    check(!parsed.has_value() || parsed->modules[0].name == "app.exe",
          "a parsed module name is a basename");
    const std::string document = tessera::diagnostics::crash_metadata_json(
        tessera::diagnostics::crash_record_metadata(record));
    check(document.find(kMarker) == std::string::npos, "the JSON document carries no marker");
}

void test_upload_redacts_before_sending() {
    const std::string report = report_with_marker();
    MemoryTransport transport;
    CrashUploadConfig config;
    config.enabled = true;
    config.consent = true;
    config.endpoint = "https://crash.example.invalid/v1";
    const auto status = upload_crash_report(config, report, transport);
    check(status == CrashUploadStatus::Sent, "an opted-in upload is sent");
    check(transport.call_count() == 1, "an opted-in upload reaches the transport exactly once");
    if (transport.call_count() == 1) {
        const auto& sent = transport.calls()[0];
        check(sent.body.find(kMarker) == std::string::npos,
              "the transmitted body does not carry the marker");
        check(sent.endpoint == config.endpoint, "the payload carries the explicit endpoint");
        check(!sent.signature.empty(), "the payload carries a grouping signature");
        check(sent.signature.find(kMarker) == std::string::npos,
              "the grouping signature carries no marker");
    }
    // A refusing transport keeps the report for a later attempt and is not a crash.
    {
        MemoryTransport refusing;
        refusing.set_accept(false);
        const auto failed = upload_crash_report(config, report, refusing);
        check(failed == CrashUploadStatus::TransportFailed,
              "a refused send is reported as TransportFailed");
        check(refusing.call_count() == 1, "a refused send is still attempted once");
    }
}

} // namespace

int main() {
    test_zero_upload_when_disabled();
    test_redaction_with_planted_marker();
    test_upload_redacts_before_sending();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "crash_upload: ok\n";
    return 0;
}
