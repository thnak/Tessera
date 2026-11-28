// SPDX-License-Identifier: MIT
//
// `FakeSecretBackend`: the deterministic in-memory test double for the secret store (ADR-078,
// issue #841). It is a test double and says so in its name; it is not a fallback and the library
// never picks it. It lives in the module for the reason `FakePermissionBackend` does: it fakes the
// module's seam, so an app's test and the contract suite share one definition of a keystore answer.
//
// Scripting: `push_failure` queues a one-shot error for the next call (any operation), and
// `set_available(false)` makes every operation fail `Unavailable`. `calls()` records the operation
// and key for each call and never a value, so a test asserts what was asked without the log ever
// holding a secret.

#pragma once

#include "tessera/appfw/secret_store.hpp"

#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

struct FakeSecretCall {
    enum class Op : std::uint8_t { Get, Set, Erase };
    Op op = Op::Get;
    SecretKey key;
};

class FakeSecretBackend final : public SecretBackend {
public:
    FakeSecretBackend() = default;

    void set_available(bool available) noexcept {
        available_ = available;
    }
    // The next call (any operation) fails with `code`; failures are consumed in order.
    void push_failure(SecretErrorCode code, std::int64_t native = 0) {
        failures_.push_back(SecretError{code, "fake", native});
    }
    void push_access_denied() {
        push_failure(SecretErrorCode::AccessDenied);
    }

    [[nodiscard]] const std::vector<FakeSecretCall>& calls() const noexcept {
        return calls_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }
    [[nodiscard]] bool contains(const SecretKey& key) const {
        return entries_.contains(std::make_pair(key.service, key.account));
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "fake-in-memory";
    }
    [[nodiscard]] bool available() noexcept override {
        return available_;
    }
    void set_max_value_bytes(std::size_t bytes) noexcept {
        max_value_bytes_ = bytes;
    }
    [[nodiscard]] std::size_t max_value_bytes() const noexcept override {
        return max_value_bytes_;
    }
    [[nodiscard]] std::expected<SecretBytes, SecretError> get(const SecretKey& key) override;
    [[nodiscard]] std::expected<void, SecretError> set(const SecretKey& key,
                                                       const SecretBytes& value) override;
    [[nodiscard]] std::expected<void, SecretError> erase(const SecretKey& key) override;

private:
    [[nodiscard]] std::expected<void, SecretError> admit(FakeSecretCall::Op op,
                                                         const SecretKey& key);

    bool available_ = true;
    std::size_t max_value_bytes_ = 16 * 1024;
    std::deque<SecretError> failures_;
    std::vector<FakeSecretCall> calls_;
    std::map<std::pair<std::string, std::string>, SecretBytes> entries_;
};

} // namespace tessera::appfw
