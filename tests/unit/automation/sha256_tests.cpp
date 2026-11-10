// SPDX-License-Identifier: MIT

#include "tessera/ui/automation/sha256.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

using tessera::ui::automation::sha256;
using tessera::ui::automation::Sha256Digest;
using tessera::ui::automation::to_hex;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "automation_sha256: " << message << "\n";
    }
    return condition;
}

std::string hex_of(std::string_view text) {
    return to_hex(sha256(text));
}

} // namespace

int main() {
    // FIPS 180-4 / NIST CAVP known answers: empty message, "abc", the 448-bit two-block message,
    // the 896-bit four-block message, and the one-million-'a' message.
    if (!expect(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                "empty digest") ||
        !expect(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "abc digest")) {
        return 1;
    }

    if (!expect(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                "448-bit digest") ||
        !expect(hex_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnop"
                       "jklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
                    "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
                "896-bit digest")) {
        return 2;
    }

    const std::string million_a(1000000, 'a');
    if (!expect(hex_of(million_a) ==
                    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                "one-million-'a' digest")) {
        return 3;
    }

    // Binary input, including NUL and bytes above 0x7f: the digest must be over the bytes, not a
    // transcoded string.
    std::uint8_t binary[64] = {};
    for (std::size_t index = 0; index < 64; ++index) {
        binary[index] = static_cast<std::uint8_t>(index);
    }
    if (!expect(to_hex(sha256(std::span<const std::uint8_t>(binary, 64))) ==
                    "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108",
                "binary span digest")) {
        return 4;
    }

    // The string overload is exactly the span overload over the same bytes, embedded NUL included.
    const std::string_view with_nul("a\0b", 3);
    const std::span<const std::uint8_t> with_nul_bytes(
        reinterpret_cast<const std::uint8_t*>(with_nul.data()), with_nul.size());
    if (!expect(sha256(with_nul) == sha256(with_nul_bytes), "string and span agree on NUL bytes") ||
        !expect(sha256("abc") == sha256(std::span<const std::uint8_t>(
                                     reinterpret_cast<const std::uint8_t*>("abc"), 3)),
                "string and span agree on text")) {
        return 5;
    }

    const Sha256Digest digest = sha256("abc");
    const std::string hex = to_hex(digest);
    if (!expect(hex.size() == 64, "hex length") ||
        !expect(hex.find_first_not_of("0123456789abcdef") == std::string::npos,
                "hex is lowercase") ||
        !expect(to_hex(sha256("abd")) != hex, "different input differs") ||
        !expect(digest.bytes[0] == 0xbaU && digest.bytes[31] == 0xadU, "digest byte order")) {
        return 6;
    }

    return 0;
}
