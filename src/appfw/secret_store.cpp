// SPDX-License-Identifier: MIT

#include "tessera/appfw/secret_store.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <utility>

namespace tessera::appfw {
namespace {

bool valid_part(const std::string& part) noexcept {
    if (part.empty() || part.size() > kMaxSecretKeyPartBytes) {
        return false;
    }
    return std::ranges::none_of(part, [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20U || u == 0x7FU;
    });
}

std::unexpected<SecretError> fail(SecretErrorCode code, std::string_view step) {
    return std::unexpected(SecretError{code, step, 0});
}

} // namespace

std::string describe_secret_error(const SecretError& error) {
    std::string text(secret_error_code_name(error.code));
    text += " (";
    text += error.step;
    text += ")";
    return text;
}

bool is_valid_secret_key(const SecretKey& key) noexcept {
    return valid_part(key.service) && valid_part(key.account);
}

SecretBytes SecretBytes::from_text(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    std::ranges::transform(text, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return SecretBytes(std::move(bytes));
}

SecretBytes::SecretBytes(SecretBytes&& other) noexcept
    : bytes_(std::move(other.bytes_)) {
    other.bytes_.clear();
}

SecretBytes& SecretBytes::operator=(SecretBytes&& other) noexcept {
    if (this != &other) {
        wipe();
        bytes_ = std::move(other.bytes_);
        other.bytes_.clear();
    }
    return *this;
}

SecretBytes::~SecretBytes() {
    wipe();
}

void SecretBytes::wipe() noexcept {
    // A volatile store so the compiler cannot drop the zeroing as a dead write.
    volatile std::byte* p = bytes_.data();
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        p[i] = std::byte{0};
    }
    bytes_.clear();
}

std::string SecretBytes::describe() const {
    return "<secret " + std::to_string(bytes_.size()) + " bytes>";
}

SecretBytes SecretBytes::duplicate() const {
    return SecretBytes(std::vector<std::byte>(bytes_));
}

struct SecretStore::Impl {
    Impl(SecretStoreConfig c, runtime::IoTaskExecutor& e, SecretBackend& b)
        : config(c),
          executor(e),
          backend(b) {}

    SecretStoreConfig config;
    runtime::IoTaskExecutor& executor;
    SecretBackend& backend;
};

SecretStore::SecretStore(SecretStoreConfig config, runtime::IoTaskExecutor& executor,
                         SecretBackend& backend)
    : impl_(std::make_shared<Impl>(config, executor, backend)) {}

SecretStore::~SecretStore() = default;

std::string_view SecretStore::backend_name() const noexcept {
    return impl_->backend.name();
}

bool SecretStore::get(SecretKey key, runtime::CancellationToken token, ValueCallback completion) {
    if (!is_valid_secret_key(key) || token.cancelled()) {
        return false;
    }
    auto outcome = std::make_shared<std::expected<SecretBytes, SecretError>>(
        fail(SecretErrorCode::Backend, "get"));
    runtime::IoTask task;
    task.work = [impl = impl_, key = std::move(key), token, outcome] {
        if (token.cancelled()) {
            *outcome = fail(SecretErrorCode::Cancelled, "get");
        } else if (!impl->backend.available()) {
            *outcome = fail(SecretErrorCode::Unavailable, "get");
        } else {
            *outcome = impl->backend.get(key);
        }
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool SecretStore::set(SecretKey key, SecretBytes value, runtime::CancellationToken token,
                      StatusCallback completion) {
    if (!is_valid_secret_key(key) || token.cancelled() ||
        value.size() > std::min(impl_->config.max_value_bytes, impl_->backend.max_value_bytes())) {
        return false;
    }
    auto outcome =
        std::make_shared<std::expected<void, SecretError>>(fail(SecretErrorCode::Backend, "set"));
    auto held = std::make_shared<SecretBytes>(std::move(value));
    runtime::IoTask task;
    task.work = [impl = impl_, key = std::move(key), token, outcome, held] {
        if (token.cancelled()) {
            *outcome = fail(SecretErrorCode::Cancelled, "set");
        } else if (!impl->backend.available()) {
            // No keystore: refuse. There is no plain-file path to fall back to (ADR-078).
            *outcome = fail(SecretErrorCode::Unavailable, "set");
        } else {
            *outcome = impl->backend.set(key, *held);
        }
        // The value's lifetime ends with the body, not with a completion that may be dropped.
        *held = SecretBytes{};
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool SecretStore::erase(SecretKey key, runtime::CancellationToken token,
                        StatusCallback completion) {
    if (!is_valid_secret_key(key) || token.cancelled()) {
        return false;
    }
    auto outcome =
        std::make_shared<std::expected<void, SecretError>>(fail(SecretErrorCode::Backend, "erase"));
    runtime::IoTask task;
    task.work = [impl = impl_, key = std::move(key), token, outcome] {
        if (token.cancelled()) {
            *outcome = fail(SecretErrorCode::Cancelled, "erase");
        } else if (!impl->backend.available()) {
            *outcome = fail(SecretErrorCode::Unavailable, "erase");
        } else {
            *outcome = impl->backend.erase(key);
        }
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

} // namespace tessera::appfw
