// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tessera::ui::automation {

// SHA-256 (FIPS 180-4) implemented in-tree. Automation IDs must be byte-identical across
// platforms and build hosts, and the project already ships no crypto dependency (ADR-013 keeps
// the statically linked set permissive and small), so the hash lives here with known-answer
// tests rather than as a new third-party library.
inline constexpr std::size_t kSha256DigestBytes = 32;

struct Sha256Digest {
    std::array<std::uint8_t, kSha256DigestBytes> bytes{};

    friend constexpr bool operator==(const Sha256Digest&, const Sha256Digest&) = default;
};

// One-shot digest of `data` (any bytes, NUL included).
[[nodiscard]] Sha256Digest sha256(std::span<const std::uint8_t> data) noexcept;

// Digest of the UTF-8 bytes of `text`. Byte order is the byte order of the caller: the salt and
// keys are already UTF-8, so this never transcodes.
[[nodiscard]] Sha256Digest sha256(std::string_view text) noexcept;

// Lowercase hex, 64 characters.
[[nodiscard]] std::string to_hex(const Sha256Digest& digest);

} // namespace tessera::ui::automation
