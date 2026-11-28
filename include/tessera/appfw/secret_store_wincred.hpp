// SPDX-License-Identifier: MIT
//
// Windows Credential Manager back end for the secret store (ADR-078 spike, issue #841). Windows
// only: the header and its source compile to nothing elsewhere. Entries are `CRED_TYPE_GENERIC`
// with `CRED_PERSIST_LOCAL_MACHINE` (never `ENTERPRISE`, which roams to a domain), target name
// "tessera:<service>/<account>". Blocking: call only through `SecretStore` on an executor thread.

#pragma once

#include "tessera/appfw/secret_store.hpp"

#if defined(_WIN32)

namespace tessera::appfw {

class WinCredSecretBackend final : public SecretBackend {
public:
    WinCredSecretBackend() = default;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "windows-credential-manager";
    }
    [[nodiscard]] bool available() noexcept override {
        return true;
    }
    // CRED_MAX_CREDENTIAL_BLOB_SIZE. A larger secret needs the wrapped-key form (ADR-078).
    [[nodiscard]] std::size_t max_value_bytes() const noexcept override {
        return 2560;
    }
    [[nodiscard]] std::expected<SecretBytes, SecretError> get(const SecretKey& key) override;
    [[nodiscard]] std::expected<void, SecretError> set(const SecretKey& key,
                                                       const SecretBytes& value) override;
    [[nodiscard]] std::expected<void, SecretError> erase(const SecretKey& key) override;
};

} // namespace tessera::appfw

#endif
