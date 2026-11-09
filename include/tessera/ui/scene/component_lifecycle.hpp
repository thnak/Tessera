// SPDX-License-Identifier: MIT

#pragma once

#include <bit>
#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::ui {

// Lifecycle seam for generated components (docs/design/compiler.md section 11):
// `useEffect` slots are bounded per component and track only a deps hash plus run bookkeeping, so
// mounting/updating/unmounting allocates nothing. Generated components call
// `effect_needs_run()` from `update_effects()` and `effect_mark_cleaned()` from `unmount()`.
struct EffectState {
    std::uint64_t deps_hash = 0;
    std::uint32_t runs = 0;
    bool active = false;

    bool operator==(const EffectState&) const = default;
};

inline constexpr std::uint64_t kEffectHashSeed = 1469598103934665603ull;
inline constexpr std::uint64_t kEffectHashPrime = 1099511628211ull;

// FNV-1a over the dependency value's bytes; deterministic and allocation-free.
constexpr std::uint64_t effect_hash_bytes(std::uint64_t hash, std::string_view bytes) noexcept {
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= kEffectHashPrime;
    }
    return hash;
}

inline std::uint64_t effect_hash(std::uint64_t hash, bool value) noexcept {
    return effect_hash_bytes(hash, value ? "1" : "0");
}

inline std::uint64_t effect_hash(std::uint64_t hash, int value) noexcept {
    return effect_hash_bytes(hash,
                             std::string_view(reinterpret_cast<const char*>(&value), sizeof value));
}

inline std::uint64_t effect_hash(std::uint64_t hash, std::int64_t value) noexcept {
    return effect_hash_bytes(hash,
                             std::string_view(reinterpret_cast<const char*>(&value), sizeof value));
}

inline std::uint64_t effect_hash(std::uint64_t hash, double value) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    return effect_hash_bytes(hash,
                             std::string_view(reinterpret_cast<const char*>(&bits), sizeof bits));
}

inline std::uint64_t effect_hash(std::uint64_t hash, std::string_view value) noexcept {
    return effect_hash_bytes(hash, value);
}

inline std::uint64_t effect_hash(std::uint64_t hash, const std::string& value) noexcept {
    return effect_hash_bytes(hash, value);
}

// True when the effect must (re)run: its dependency signature changed, or it is not active yet.
// Updates the stored signature and increments the run counter; the caller invokes cleanup first
// when `active` was true and the hash differs.
inline bool effect_needs_run(EffectState& state, std::uint64_t deps_hash) noexcept {
    if (state.active && state.deps_hash == deps_hash) {
        return false;
    }
    state.deps_hash = deps_hash;
    ++state.runs;
    state.active = true;
    return true;
}

inline void effect_mark_cleaned(EffectState& state) noexcept {
    state.active = false;
}

} // namespace tessera::ui
