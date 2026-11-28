// SPDX-License-Identifier: MIT

#include "tessera/appfw/secret_store_fake.hpp"

#include <utility>

namespace tessera::appfw {

std::expected<void, SecretError> FakeSecretBackend::admit(FakeSecretCall::Op op,
                                                          const SecretKey& key) {
    calls_.push_back(FakeSecretCall{op, key});
    if (!failures_.empty()) {
        SecretError error = failures_.front();
        failures_.pop_front();
        return std::unexpected(error);
    }
    if (!available_) {
        return std::unexpected(SecretError{SecretErrorCode::Unavailable, "fake", 0});
    }
    return {};
}

std::expected<SecretBytes, SecretError> FakeSecretBackend::get(const SecretKey& key) {
    if (auto admitted = admit(FakeSecretCall::Op::Get, key); !admitted) {
        return std::unexpected(admitted.error());
    }
    const auto it = entries_.find(std::make_pair(key.service, key.account));
    if (it == entries_.end()) {
        return std::unexpected(SecretError{SecretErrorCode::NotFound, "get", 0});
    }
    return it->second.duplicate();
}

std::expected<void, SecretError> FakeSecretBackend::set(const SecretKey& key,
                                                        const SecretBytes& value) {
    if (auto admitted = admit(FakeSecretCall::Op::Set, key); !admitted) {
        return admitted;
    }
    entries_.insert_or_assign(std::make_pair(key.service, key.account), value.duplicate());
    return {};
}

std::expected<void, SecretError> FakeSecretBackend::erase(const SecretKey& key) {
    if (auto admitted = admit(FakeSecretCall::Op::Erase, key); !admitted) {
        return admitted;
    }
    entries_.erase(std::make_pair(key.service, key.account));
    return {};
}

} // namespace tessera::appfw
