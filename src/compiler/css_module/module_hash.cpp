// SPDX-License-Identifier: MIT

#include "tessera/compiler/css_module/module_scan.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace tessera::compiler::css_module {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;
// The spec's vectors are the low 24 bits of the 64-bit hash, so the name stays six characters.
constexpr std::uint64_t kHashMask = 0xFFFFFFULL;
constexpr std::size_t kHashDigits = 6;
constexpr char kHexDigits[] = "0123456789abcdef";

} // namespace

std::string module_hash(std::string_view project_relative_path) {
    std::uint64_t hash = kFnvOffsetBasis;
    for (const char c : project_relative_path) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= kFnvPrime;
    }
    const std::uint64_t truncated = hash & kHashMask;
    std::string out(kHashDigits, '0');
    for (std::size_t i = 0; i < kHashDigits; ++i) {
        const auto nibble = static_cast<std::size_t>((truncated >> (i * 4)) & 0xFULL);
        out[kHashDigits - 1 - i] = kHexDigits[nibble];
    }
    return out;
}

std::string scoped_name(std::string_view local, std::string_view hash) {
    std::string out;
    out.reserve(local.size() + 1 + hash.size());
    out.append(local);
    out.push_back('_');
    out.append(hash);
    return out;
}

} // namespace tessera::compiler::css_module
