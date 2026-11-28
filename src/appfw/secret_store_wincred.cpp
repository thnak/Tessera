// SPDX-License-Identifier: MIT

#include "tessera/appfw/secret_store_wincred.hpp"

#if defined(_WIN32)

#include <cstddef>
#include <string>
#include <utility>
#include <vector>
// windows.h must precede wincred.h (the latter needs its types); keep clang-format from sorting
// them.
// clang-format off
#include <windows.h>
#include <wincred.h>
// clang-format on

namespace tessera::appfw {
namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    }
    return out;
}

std::wstring target_name(const SecretKey& key) {
    return L"tessera:" + widen(key.service) + L"/" + widen(key.account);
}

std::unexpected<SecretError> from_last_error(std::string_view step) {
    const DWORD code = GetLastError();
    SecretErrorCode mapped = SecretErrorCode::Backend;
    if (code == ERROR_NOT_FOUND) {
        mapped = SecretErrorCode::NotFound;
    } else if (code == ERROR_ACCESS_DENIED || code == ERROR_LOGON_FAILURE ||
               code == ERROR_NO_SUCH_LOGON_SESSION) {
        mapped = SecretErrorCode::AccessDenied;
    }
    return std::unexpected(SecretError{mapped, step, static_cast<std::int64_t>(code)});
}

} // namespace

std::expected<SecretBytes, SecretError> WinCredSecretBackend::get(const SecretKey& key) {
    const std::wstring target = target_name(key);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        return from_last_error("get");
    }
    const auto* data = reinterpret_cast<const std::byte*>(cred->CredentialBlob);
    SecretBytes out{std::vector<std::byte>(data, data + cred->CredentialBlobSize)};
    // CredFree does not zero the blob, so wipe the OS copy we hold before releasing it.
    SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    return out;
}

std::expected<void, SecretError> WinCredSecretBackend::set(const SecretKey& key,
                                                           const SecretBytes& value) {
    std::wstring target = target_name(key);
    std::wstring user = widen(key.account);
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = target.data();
    cred.UserName = user.data();
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.CredentialBlobSize = static_cast<DWORD>(value.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<std::byte*>(value.bytes().data()));
    if (!CredWriteW(&cred, 0)) {
        return from_last_error("set");
    }
    return {};
}

std::expected<void, SecretError> WinCredSecretBackend::erase(const SecretKey& key) {
    const std::wstring target = target_name(key);
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
        return from_last_error("erase");
    }
    return {};
}

} // namespace tessera::appfw

#endif
