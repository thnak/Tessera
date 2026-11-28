// SPDX-License-Identifier: MIT

#pragma once

#include <bit>
#include <cstdint>
#include <string_view>

namespace tessera::ui::pending {

// Folds `value` into the running signature `word` (FNV-1a over the eight bytes of `value`).
[[nodiscard]] constexpr std::uint64_t mix_signature(std::uint64_t word,
                                                    std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        word = (word ^ ((value >> shift) & 0xffu)) * 1099511628211ull;
    }
    return word;
}

// What a suspending read needs to know about one resource at one instant
// (docs/specs/pending-ui-v1.md section 3.2): whether a fetch is in flight, whether data is
// published, whether the last fetch failed, and the epoch a completion must still match. Plain
// words, no key and no value, so reading and copying it never allocates.
struct ResourceState {
    std::uint64_t epoch = 0;
    bool loading = false;
    bool has_data = false;
    bool failed = false;
    // The published scalar of a typed resource (`Resource::type`, pending-ui-v1.md section 3.4):
    // `number` for an int, float or bool, `text` for a string. A source that has no data, or no
    // scalar of that kind, leaves them at the zero value, which is what an absent `.data` reads.
    // `text` is a view the source keeps alive until its next publish; nothing here allocates.
    double number = 0.0;
    std::string_view text{};
    // The text of the last failure (`res::<name>.error`, a string): the store's error message.
    // Empty while the resource has not failed. A view the source keeps alive until its next
    // publish.
    std::string_view error{};

    // The `res::<name>.status` value (async-data-v1.md section 1, lower case). `Stale` is never
    // produced (pending-ui-v1.md section 12, question 1, resolved: the epoch is what is read).
    [[nodiscard]] constexpr const char* status() const noexcept {
        if (loading) {
            return has_data ? "refreshing" : "loading";
        }
        if (failed) {
            return "error";
        }
        return has_data ? "success" : "idle";
    }

    // One word that changes when anything a binding can read here changes (status, epoch, scalar,
    // text, error). A generated `pending_sync()` compares it with the last one it saw to decide
    // whether the bindings that read `res::` are dirty (pending-ui-v1.md section 10.1). Text and
    // error contribute their address and length, not their bytes: a source republishes through a
    // new epoch, and hashing a long text every frame would cost more than the update it avoids.
    [[nodiscard]] std::uint64_t signature() const noexcept {
        std::uint64_t word = mix_signature(1469598103934665603ull, epoch);
        word = mix_signature(word, (loading ? 1u : 0u) | (has_data ? 2u : 0u) | (failed ? 4u : 0u));
        word = mix_signature(word, std::bit_cast<std::uint64_t>(number));
        word = mix_signature(word, reinterpret_cast<std::uintptr_t>(text.data()));
        word = mix_signature(word, text.size());
        word = mix_signature(word, reinterpret_cast<std::uintptr_t>(error.data()));
        return mix_signature(word, error.size());
    }

    // A suspending read: a fetch is in flight and there is nothing to show yet.
    [[nodiscard]] constexpr bool suspends() const noexcept {
        return loading && !has_data;
    }
    // Refreshing: a fetch is in flight and the previous data is still shown.
    [[nodiscard]] constexpr bool refreshing() const noexcept {
        return loading && has_data;
    }
};

// The narrow seam between a Suspense boundary and whatever owns the resource. It is a non-owning
// (object, function) pair rather than a virtual interface, so it costs no allocation, no RTTI and
// no vtable in a generated component, and an unbound view reads as an idle resource (nothing
// suspends) instead of crashing.
//
// Two implementations ship: the deterministic `FakeResource` (evaluator, tests, the Studio mock
// worker) and `StoreResource` over the data store. Any type with
// `ResourceState state() const noexcept` can be viewed; the viewed object must outlive the view.
class ResourceView {
public:
    constexpr ResourceView() noexcept = default;

    template<typename Source>
    [[nodiscard]] static constexpr ResourceView of(const Source& source) noexcept {
        ResourceView view;
        view.self_ = &source;
        view.read_ = [](const void* self) noexcept {
            return static_cast<const Source*>(self)->state();
        };
        return view;
    }

    [[nodiscard]] constexpr bool bound() const noexcept {
        return read_ != nullptr;
    }

    [[nodiscard]] ResourceState read() const noexcept {
        return read_ != nullptr ? read_(self_) : ResourceState{};
    }

private:
    const void* self_ = nullptr;
    ResourceState (*read_)(const void*) noexcept = nullptr;
};

} // namespace tessera::ui::pending
