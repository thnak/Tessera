// SPDX-License-Identifier: MIT

#include "tessera/ui/automation/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tessera::ui::automation {
namespace {

// FIPS 180-4 section 4.2.2 (first 32 bits of the fractional parts of the cube roots of the first
// 64 primes) and section 5.3.3 (square roots of the first 8 primes).
constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

constexpr std::array<std::uint32_t, 8> kInitialState = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

constexpr std::size_t kBlockBytes = 64;
constexpr std::size_t kScheduleWords = 64;

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

void compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, kScheduleWords> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
        const std::size_t offset = index * 4;
        schedule[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                          (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
                          (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
                          static_cast<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < kScheduleWords; ++index) {
        const std::uint32_t word_15 = schedule[index - 15];
        const std::uint32_t word_2 = schedule[index - 2];
        const std::uint32_t small_0 =
            rotate_right(word_15, 7) ^ rotate_right(word_15, 18) ^ (word_15 >> 3U);
        const std::uint32_t small_1 =
            rotate_right(word_2, 17) ^ rotate_right(word_2, 19) ^ (word_2 >> 10U);
        schedule[index] = schedule[index - 16] + small_0 + schedule[index - 7] + small_1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (std::size_t index = 0; index < kScheduleWords; ++index) {
        const std::uint32_t big_1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp_1 = h + big_1 + choose + kRoundConstants[index] + schedule[index];
        const std::uint32_t big_0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp_2 = big_0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp_1;
        d = c;
        c = b;
        b = a;
        a = temp_1 + temp_2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

Sha256Digest digest_of(const std::array<std::uint32_t, 8>& state) noexcept {
    Sha256Digest digest{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        const std::size_t offset = index * 4;
        digest.bytes[offset] = static_cast<std::uint8_t>(state[index] >> 24U);
        digest.bytes[offset + 1] = static_cast<std::uint8_t>(state[index] >> 16U);
        digest.bytes[offset + 2] = static_cast<std::uint8_t>(state[index] >> 8U);
        digest.bytes[offset + 3] = static_cast<std::uint8_t>(state[index]);
    }
    return digest;
}

} // namespace

Sha256Digest sha256(std::span<const std::uint8_t> data) noexcept {
    std::array<std::uint32_t, 8> state = kInitialState;
    std::size_t offset = 0;
    for (; offset + kBlockBytes <= data.size(); offset += kBlockBytes) {
        compress(state, data.data() + offset);
    }

    // A message is padded with 0x80, zero bytes, and its length in bits as a big-endian 64-bit
    // word in the final block; a remainder of 56 bytes or more needs one extra block.
    const std::size_t remainder = data.size() - offset;
    std::array<std::uint8_t, kBlockBytes> tail{};
    for (std::size_t index = 0; index < remainder; ++index) {
        tail[index] = data[offset + index];
    }
    tail[remainder] = 0x80U;
    if (remainder >= 56) {
        compress(state, tail.data());
        tail.fill(0);
    }
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    for (std::size_t index = 0; index < 8; ++index) {
        tail[kBlockBytes - 1 - index] = static_cast<std::uint8_t>(bit_length >> (index * 8U));
    }
    compress(state, tail.data());
    return digest_of(state);
}

Sha256Digest sha256(std::string_view text) noexcept {
    return sha256(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                                text.size()));
}

std::string to_hex(const Sha256Digest& digest) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.bytes.size() * 2);
    for (const std::uint8_t byte : digest.bytes) {
        hex.push_back(kDigits[byte >> 4U]);
        hex.push_back(kDigits[byte & 0x0FU]);
    }
    return hex;
}

} // namespace tessera::ui::automation
