// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace tessera::ui {

// The dirty mask of a generated component (ADR-056): one bit per state cell, animated value and
// prop. A setter sets its cell's bit; `apply_state()` re-evaluates a binding only when the
// binding's dependency mask intersects the mask. The storage is one `std::uint64_t` word up to 64
// cells and an array of words above that, so the generated code has the same shape for every
// component and only the template argument changes. A member, never heap storage: setting a bit
// allocates nothing.
template<std::size_t Bits> class DirtyMask {
public:
    static constexpr std::size_t kBits = Bits == 0 ? 1 : Bits;
    static constexpr std::size_t kWords = (kBits + 63) / 64;

    constexpr DirtyMask() = default;

    [[nodiscard]] static constexpr DirtyMask of(std::initializer_list<std::size_t> bits) {
        DirtyMask mask;
        for (const std::size_t bit : bits) {
            mask.set(bit);
        }
        return mask;
    }

    // Every cell dirty: the first build, and a hand call of `update()`.
    [[nodiscard]] static constexpr DirtyMask all() {
        DirtyMask mask;
        for (std::uint64_t& word : mask.words_) {
            word = ~std::uint64_t{0};
        }
        return mask;
    }

    constexpr void set(std::size_t bit) {
        if (bit < kBits) {
            words_[bit / 64] |= std::uint64_t{1} << (bit % 64);
        }
    }

    constexpr void set_all() {
        *this = all();
    }

    constexpr void clear() {
        for (std::uint64_t& word : words_) {
            word = 0;
        }
    }

    // Clears the bits of `consumed`: a bit a setter raised after `consumed` was taken survives.
    constexpr void clear(const DirtyMask& consumed) {
        for (std::size_t i = 0; i < kWords; ++i) {
            words_[i] &= ~consumed.words_[i];
        }
    }

    [[nodiscard]] constexpr bool test(std::size_t bit) const {
        return bit < kBits && ((words_[bit / 64] >> (bit % 64)) & 1U) != 0;
    }

    [[nodiscard]] constexpr bool any() const {
        for (const std::uint64_t word : words_) {
            if (word != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr bool intersects(const DirtyMask& other) const {
        for (std::size_t i = 0; i < kWords; ++i) {
            if ((words_[i] & other.words_[i]) != 0) {
                return true;
            }
        }
        return false;
    }

private:
    std::array<std::uint64_t, kWords> words_{};
};

} // namespace tessera::ui

// Test seam for the per-binding tests (ADR-056 slice 2): a generated component calls
// `TESSERA_BINDING_RAN` at the top of every dependency-gated binding it re-evaluates. Without
// `TESSERA_BINDING_TRACE` defined it is an empty statement, so production code carries nothing.
#ifdef TESSERA_BINDING_TRACE
namespace tessera::ui {
using BindingTraceHook = void (*)(const void* component, const char* binding);
inline BindingTraceHook g_binding_trace_hook = nullptr;
inline void binding_ran(const void* component, const char* binding) {
    if (g_binding_trace_hook != nullptr) {
        g_binding_trace_hook(component, binding);
    }
}
} // namespace tessera::ui
#define TESSERA_BINDING_RAN(component, binding) ::tessera::ui::binding_ran((component), (binding))
#else
#define TESSERA_BINDING_RAN(component, binding) ((void)0)
#endif
