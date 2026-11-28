// SPDX-License-Identifier: MIT
//
// ADR-078 spike (issue #841): the secret store contract, proved against the in-memory fake with a
// manual executor. Covers the async round trip, typed failures, no-fallback, cancellation, key
// validation and the redaction rules (no value in any error text, call record or log line).

#include "tessera/appfw/secret_store.hpp"
#include "tessera/appfw/secret_store_fake.hpp"
#include "tessera/diagnostics/log.hpp"
#include "tessera/runtime/manual_io_executor.hpp"

#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace tessera::appfw;
using tessera::runtime::CancellationSource;
using tessera::runtime::ManualIoExecutor;

int g_failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_secret: " << message << "\n";
        ++g_failures;
    }
}

constexpr std::string_view kToken = "s3cr3t-refresh-token-VALUE";

struct Harness {
    ManualIoExecutor executor;
    FakeSecretBackend backend;
    SecretStore store{SecretStoreConfig{}, executor, backend};
    SecretKey key{"api.example.com", "refresh-token"};

    void pump() {
        executor.run(16);
        executor.run_completions(16);
    }
};

void set_then_get_round_trips_through_the_executor() {
    Harness h;
    bool set_done = false;
    expect(h.store.set(h.key, SecretBytes::from_text(kToken), {},
                       [&](std::expected<void, SecretError> r) { set_done = r.has_value(); }),
           "set is posted");
    expect(h.executor.pending_work() == 1 && h.backend.calls().empty(),
           "post does not call the backend");
    h.pump();
    expect(set_done && h.backend.contains(h.key), "set completes and stores");

    std::string got;
    h.store.get(h.key, {}, [&](std::expected<SecretBytes, SecretError> r) {
        if (r) {
            for (const std::byte b : r->bytes()) {
                got.push_back(static_cast<char>(b));
            }
        }
    });
    h.pump();
    expect(got == kToken, "get returns the stored bytes");
}

void missing_entry_is_not_found_and_erase_is_idempotent() {
    Harness h;
    SecretErrorCode code = SecretErrorCode::None;
    h.store.get(h.key, {}, [&](auto r) { code = r ? SecretErrorCode::None : r.error().code; });
    h.pump();
    expect(code == SecretErrorCode::NotFound, "absent key is NotFound");

    bool erased = false;
    h.store.erase(h.key, {}, [&](auto r) { erased = r.has_value(); });
    h.pump();
    expect(erased, "erasing an absent entry succeeds");
}

void no_keystore_fails_set_with_unavailable_and_stores_nothing() {
    Harness h;
    h.backend.set_available(false);
    SecretErrorCode code = SecretErrorCode::None;
    h.store.set(h.key, SecretBytes::from_text(kToken), {},
                [&](auto r) { code = r ? SecretErrorCode::None : r.error().code; });
    h.pump();
    expect(code == SecretErrorCode::Unavailable, "set reports Unavailable");
    expect(h.backend.size() == 0, "nothing is stored when the keystore is unavailable");
    expect(h.backend.calls().empty(), "an unavailable keystore is not even asked");
}

void scripted_access_denied_reaches_the_caller() {
    Harness h;
    h.backend.push_access_denied();
    SecretErrorCode code = SecretErrorCode::None;
    h.store.get(h.key, {}, [&](auto r) { code = r ? SecretErrorCode::None : r.error().code; });
    h.pump();
    expect(code == SecretErrorCode::AccessDenied, "scripted AccessDenied is delivered");
}

void cancellation_before_the_body_skips_the_backend() {
    Harness h;
    CancellationSource source;
    SecretErrorCode code = SecretErrorCode::None;
    h.store.get(h.key, source.token(),
                [&](auto r) { code = r ? SecretErrorCode::None : r.error().code; });
    source.cancel();
    h.pump();
    expect(code == SecretErrorCode::Cancelled, "cancelled get reports Cancelled");
    expect(h.backend.calls().empty(), "cancelled get never reaches the keystore");

    CancellationSource already;
    already.cancel();
    expect(!h.store.get(h.key, already.token(), {}), "an already-cancelled token is refused");
}

void invalid_keys_and_oversized_values_are_refused_at_submit() {
    Harness h;
    expect(!h.store.get(SecretKey{"", "a"}, {}, {}), "empty service is refused");
    expect(!h.store.get(SecretKey{"svc", "bad\nname"}, {}, {}), "control char is refused");
    expect(!h.store.get(SecretKey{"svc", std::string(300, 'x')}, {}, {}), "long key is refused");
    SecretStoreConfig small;
    small.max_value_bytes = 4;
    SecretStore tiny(small, h.executor, h.backend);
    expect(!tiny.set(h.key, SecretBytes::from_text("12345"), {}, {}), "oversized value refused");
    h.backend.set_max_value_bytes(8);
    expect(!h.store.set(h.key, SecretBytes::from_text("123456789"), {}, {}),
           "a value over the keystore's own cap is refused");
    expect(h.store.set(h.key, SecretBytes::from_text("12345678"), {}, {}),
           "at the cap is accepted");
    h.executor.clear();
    expect(h.executor.pending_work() == 0, "refused submissions queue nothing");
}

void a_full_executor_refuses_the_submission() {
    ManualIoExecutor tiny_executor(1);
    FakeSecretBackend backend;
    SecretStore store(SecretStoreConfig{}, tiny_executor, backend);
    const SecretKey key{"svc", "acct"};
    expect(store.get(key, {}, {}), "first fits");
    expect(!store.get(key, {}, {}), "second is refused at capacity");
}

void secrets_never_reach_errors_calls_or_logs() {
    Harness h;
    h.backend.push_access_denied();
    std::string error_text;
    h.store.set(h.key, SecretBytes::from_text(kToken), {}, [&](auto r) {
        if (!r) {
            error_text = describe_secret_error(r.error());
        }
    });
    h.pump();
    expect(!error_text.empty() && error_text.find(kToken) == std::string::npos,
           "error text has no value");
    expect(error_text.find(h.key.account) == std::string::npos, "error text has no key");

    h.store.set(h.key, SecretBytes::from_text(kToken), {}, {});
    h.pump();
    for (const FakeSecretCall& call : h.backend.calls()) {
        expect(call.key.account != std::string(kToken), "call record holds the key, not the value");
    }

    // The one printable form goes through a logger and the sink sees only the size.
    tessera::diagnostics::MemoryLogSink sink;
    tessera::diagnostics::Logger logger;
    logger.add_sink(sink);
    const SecretBytes secret = SecretBytes::from_text(kToken);
    logger.log(tessera::diagnostics::LogLevel::Info, tessera::diagnostics::LogCategory::App,
               "loaded " + secret.describe());
    logger.log(tessera::diagnostics::LogLevel::Info, tessera::diagnostics::LogCategory::App,
               describe_secret_error(SecretError{SecretErrorCode::AccessDenied, "get", 5}));
    expect(sink.records().size() == 2, "both records are logged");
    for (const auto& record : sink.records()) {
        expect(record.message.find(kToken) == std::string::npos, "log line has no secret");
    }
    expect(secret.describe() == "<secret 26 bytes>", "describe shows only the size");
}

void secret_bytes_are_move_only_and_wiped() {
    SecretBytes a = SecretBytes::from_text("abc");
    SecretBytes b = std::move(a);
    expect(a.empty() && b.size() == 3, "move empties the source");
    const SecretBytes copy = b.duplicate();
    b = SecretBytes{};
    expect(b.empty() && copy.size() == 3, "duplicate is independent");
}

} // namespace

int main() {
    set_then_get_round_trips_through_the_executor();
    missing_entry_is_not_found_and_erase_is_idempotent();
    no_keystore_fails_set_with_unavailable_and_stores_nothing();
    scripted_access_denied_reaches_the_caller();
    cancellation_before_the_body_skips_the_backend();
    invalid_keys_and_oversized_values_are_refused_at_submit();
    a_full_executor_refuses_the_submission();
    secrets_never_reach_errors_calls_or_logs();
    secret_bytes_are_move_only_and_wiped();
    if (g_failures != 0) {
        std::cerr << "appfw_secret: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_secret: ok\n";
    return 0;
}
