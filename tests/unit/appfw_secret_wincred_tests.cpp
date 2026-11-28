// SPDX-License-Identifier: MIT
//
// ADR-078 spike (issue #841): a real round trip through Windows Credential Manager with a unique
// key, erased at the end. Windows only. The entry is per-user and local, never roamed.

#include "tessera/appfw/secret_store.hpp"
#include "tessera/appfw/secret_store_wincred.hpp"

#include <iostream>

int main() {
    using namespace tessera::appfw;
    WinCredSecretBackend backend;
    const SecretKey key{"tessera-spike-841", "wincred-test"};
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::cerr << "appfw_secret_wincred: " << what << "\n";
            ++failures;
        }
    };
    check(backend.erase(key).has_value(), "pre-clean erase");
    check(backend.get(key).error().code == SecretErrorCode::NotFound, "absent is NotFound");
    const bool wrote = backend.set(key, SecretBytes::from_text("round-trip-value")).has_value();
    check(wrote, "set");
    if (wrote) {
        auto got = backend.get(key);
        check(got.has_value() && got->size() == 16, "get returns the same size");
    }
    check(backend.set(key, SecretBytes::from_text("v2")).has_value(), "overwrite");
    auto second = backend.get(key);
    check(second.has_value() && second->size() == 2, "overwrite is visible");
    check(backend.erase(key).has_value(), "erase");
    check(backend.get(key).error().code == SecretErrorCode::NotFound, "erased is NotFound");
    check(backend.erase(key).has_value(), "erase twice is idempotent");
    if (failures != 0) {
        return 1;
    }
    std::cout << "appfw_secret_wincred: ok\n";
    return 0;
}
