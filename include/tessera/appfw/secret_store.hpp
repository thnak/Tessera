// SPDX-License-Identifier: MIT
//
// The secret store seam and its async front end (ADR-078, issue #841). Spike evidence only: the ADR
// is Proposed, so this lands off the default path and no platform back end is wired in.
//
// **Shape.** A `SecretBackend` is the blocking platform seam (Credential Manager, Secret Service,
// Keychain, Keystore): it is called only on an executor thread and names no platform type. A
// `SecretStore` is what the app holds: `get`/`set`/`erase` post one blocking body to the
// `IoTaskExecutor` and deliver a `std::expected` to the draining thread, the way `Storage` does.
//
// **Rules.**
//   1. A secret is bytes in a `SecretBytes`: move-only, wiped on destruction, and its only
//      printable form is `describe()` ("<secret N bytes>"). There is no conversion to a string
//      view an author could pass to a logger by accident.
//   2. An error carries a code, a fixed step name and a native code. Never a key, never a value.
//   3. No insecure fallback: with no keystore, `set` fails `Unavailable`. This header has no
//      plain-file implementation and the fake is explicit about being a test double.
//   4. The UI thread never calls a backend; `post()` only enqueues (ADR-021).

#pragma once

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/io_task_executor.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

enum class SecretErrorCode : std::uint8_t {
    None = 0,
    NotFound,    // no entry under that key
    Unavailable, // no keystore here (Wasm, headless Linux with no Secret Service): never a fallback
    AccessDenied, // the keystore refused (locked, user declined, ACL)
    InvalidKey,   // empty, too long or containing a control character
    TooLarge,     // the value exceeds the configured cap
    Cancelled,    // the caller cancelled before the keystore was called
    Busy,         // the executor refused the submission
    Backend,      // the keystore failed for another reason (see `native`)
};

[[nodiscard]] constexpr std::string_view secret_error_code_name(SecretErrorCode code) noexcept {
    switch (code) {
    case SecretErrorCode::None:
        return "none";
    case SecretErrorCode::NotFound:
        return "not found";
    case SecretErrorCode::Unavailable:
        return "unavailable";
    case SecretErrorCode::AccessDenied:
        return "access denied";
    case SecretErrorCode::InvalidKey:
        return "invalid key";
    case SecretErrorCode::TooLarge:
        return "too large";
    case SecretErrorCode::Cancelled:
        return "cancelled";
    case SecretErrorCode::Busy:
        return "busy";
    case SecretErrorCode::Backend:
        return "backend";
    }
    return "unknown";
}

struct SecretError {
    SecretErrorCode code = SecretErrorCode::Backend;
    // The step that produced it ("validate", "submit", "get", "set", "erase"). A literal, so it can
    // never carry caller data.
    std::string_view step;
    std::int64_t native = 0;

    [[nodiscard]] bool operator==(const SecretError&) const = default;
};

// The text safe to log for an error: code and step only.
[[nodiscard]] std::string describe_secret_error(const SecretError& error);

// A key names one secret: `service` is the app-chosen namespace ("api.example.com"), `account` the
// entry inside it ("refresh-token"). Neither is secret, but both are kept out of error text.
struct SecretKey {
    std::string service;
    std::string account;
};

inline constexpr std::size_t kMaxSecretKeyPartBytes = 256;

[[nodiscard]] bool is_valid_secret_key(const SecretKey& key) noexcept;

// Move-only secret bytes, zeroed on destruction and on move-from.
class SecretBytes {
public:
    SecretBytes() = default;
    explicit SecretBytes(std::vector<std::byte> bytes) noexcept
        : bytes_(std::move(bytes)) {}
    static SecretBytes from_text(std::string_view text);

    SecretBytes(const SecretBytes&) = delete;
    SecretBytes& operator=(const SecretBytes&) = delete;
    SecretBytes(SecretBytes&& other) noexcept;
    SecretBytes& operator=(SecretBytes&& other) noexcept;
    ~SecretBytes();

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return bytes_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return bytes_.empty();
    }
    // "<secret N bytes>": the only form meant for logs and diagnostics.
    [[nodiscard]] std::string describe() const;
    // An explicit, greppable copy for a caller that must hand the secret on (an Authorization
    // header). The caller owns the lifetime of the copy.
    [[nodiscard]] SecretBytes duplicate() const;

private:
    void wipe() noexcept;
    std::vector<std::byte> bytes_;
};

// The blocking platform seam. Called on an executor thread only; must not throw; must not call
// back into the UI. `available()` is a cheap, non-prompting probe.
class SecretBackend {
public:
    virtual ~SecretBackend() = default;

    SecretBackend(const SecretBackend&) = delete;
    SecretBackend& operator=(const SecretBackend&) = delete;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool available() noexcept = 0;
    // The largest value this keystore holds (Credential Manager: 2560 bytes). The store refuses a
    // larger `set` with `TooLarge` instead of letting the OS truncate or fail obscurely.
    [[nodiscard]] virtual std::size_t max_value_bytes() const noexcept {
        return 16 * 1024;
    }
    [[nodiscard]] virtual std::expected<SecretBytes, SecretError> get(const SecretKey& key) = 0;
    [[nodiscard]] virtual std::expected<void, SecretError> set(const SecretKey& key,
                                                               const SecretBytes& value) = 0;
    // Erasing an absent entry succeeds (idempotent, like `Storage::remove`).
    [[nodiscard]] virtual std::expected<void, SecretError> erase(const SecretKey& key) = 0;

protected:
    SecretBackend() = default;
};

struct SecretStoreConfig {
    std::size_t max_value_bytes = 16 * 1024;
};

class SecretStore {
public:
    using ValueCallback = std::function<void(std::expected<SecretBytes, SecretError>)>;
    using StatusCallback = std::function<void(std::expected<void, SecretError>)>;

    // `executor` and `backend` are borrowed and must outlive the store and every posted task.
    SecretStore(SecretStoreConfig config, runtime::IoTaskExecutor& executor,
                SecretBackend& backend);
    ~SecretStore();

    SecretStore(const SecretStore&) = delete;
    SecretStore& operator=(const SecretStore&) = delete;

    // Each returns false, with no completion, for an invalid key, an already-cancelled token or a
    // refused submission; otherwise the completion runs once on the draining thread.
    bool get(SecretKey key, runtime::CancellationToken token, ValueCallback completion);
    bool set(SecretKey key, SecretBytes value, runtime::CancellationToken token,
             StatusCallback completion);
    bool erase(SecretKey key, runtime::CancellationToken token, StatusCallback completion);

    [[nodiscard]] std::string_view backend_name() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace tessera::appfw
