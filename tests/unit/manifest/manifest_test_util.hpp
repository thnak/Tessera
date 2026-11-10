// SPDX-License-Identifier: MIT

// Shared helpers for the shader-manifest tests. The tests are plain executables with a failure
// counter, matching the other tests/unit lanes: no framework, no wall clock, no filesystem writes.

#pragma once

#include "tessera/schema/manifest.hpp"

#include <expected>
#include <iostream>
#include <string>
#include <string_view>

namespace manifest_test {

class Checker {
public:
    void check(bool condition, std::string_view message) {
        if (!condition) {
            ++failures_;
            std::cerr << "manifest: " << message << "\n";
        }
    }

    [[nodiscard]] int failures() const noexcept {
        return failures_;
    }

    // Test executables return this; ctest treats non-zero as failure.
    [[nodiscard]] int finish() const noexcept {
        return failures_ == 0 ? 0 : 1;
    }

private:
    int failures_ = 0;
};

inline std::string replace_first(std::string_view text, std::string_view needle,
                                 std::string_view replacement) {
    std::string result(text);
    const std::size_t position = result.find(needle);
    if (position != std::string::npos) {
        result.replace(position, needle.size(), replacement);
    }
    return result;
}

inline std::string erase_first(std::string_view text, std::string_view needle) {
    std::string result(text);
    const std::size_t position = result.find(needle);
    if (position != std::string::npos) {
        result.erase(position, needle.size());
    }
    return result;
}

inline void
expect_ok(Checker& checker,
          const std::expected<tessera::schema::Manifest, tessera::schema::ValidationError>& result,
          std::string_view label) {
    if (!result) {
        checker.check(false, std::string(label) + ": unexpected error " +
                                 std::string(tessera::schema::to_string(result.error().code)) +
                                 " at " + result.error().path);
        return;
    }
    checker.check(true, label);
}

inline void expect_error(
    Checker& checker,
    const std::expected<tessera::schema::Manifest, tessera::schema::ValidationError>& result,
    tessera::schema::ValidationErrorCode code, std::string_view path, std::string_view label) {
    if (result) {
        checker.check(false, std::string(label) + ": expected an error but parsing succeeded");
        return;
    }
    checker.check(result.error().code == code,
                  std::string(label) + ": expected code " +
                      std::string(tessera::schema::to_string(code)) + ", got " +
                      std::string(tessera::schema::to_string(result.error().code)));
    checker.check(result.error().path == path, std::string(label) + ": expected path " +
                                                   std::string(path) + ", got " +
                                                   result.error().path);
}

} // namespace manifest_test
