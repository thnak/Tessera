// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tessera::ui {

// Stable accelerator id returned by registration. 0 is invalid; ids are never reused, even after a
// binding is unregistered or the registry is cleared (until the 32-bit counter wraps, which is far
// beyond any real session).
using BindingId = std::uint32_t;
inline constexpr BindingId kInvalidBindingId = 0;

// A keyboard shortcut chord. Modifier matching ignores the lock modifiers (CapsLock/NumLock) and
// treats the ctrl/alt/shift/meta bits as an unordered set, so ctrl+shift and shift+ctrl are the
// same chord.
struct KeyChord {
    KeyCode key = 0;
    Modifier modifiers = Modifier::None;
};

[[nodiscard]] Modifier normalize_chord_modifiers(Modifier modifiers);
[[nodiscard]] KeyChord normalize_chord(KeyChord chord);

// Invoked for a KeyDown that reached the registry and matched. The callback must not block; it may
// register/remove bindings, which is safe during dispatch (the in-flight call uses a bounded
// snapshot and skips entries removed before they run).
using AcceleratorCallback = void (*)(void* user, const InputEvent& event);

// Bounded, deterministic accelerator registry. Lookup is a constant-bounded scan (at most
// kMaxBindings = 64 entries, no allocation), so it is effectively O(1) in the hot path. Only
// `KeyDown` events fire bindings; `KeyUp` is ignored.
//
// Matching rules:
// - Default is an exact modifier match: a Ctrl+S binding never fires for Ctrl+Shift+S.
// - A binding registered with `allow_superset` also fires when the event carries extra modifiers,
//   as long as it contains the binding's modifiers (Ctrl+K binds Ctrl+Shift+K and Ctrl+Alt+K).
//   Broad bindings are evaluated after all exact bindings, even when their modifiers match exactly,
//   so a non-broad Ctrl+K binding wins over a broad Ctrl+K binding; within a class the first
//   registration wins.
// - At most one binding fires per event.
// - A repeat event (`InputEvent::repeat`) only fires bindings registered with `repeat = true`;
//   normal bindings fire once per physical press.
class KeyBindingRegistry {
public:
    static constexpr std::size_t kMaxBindings = 64;

    // Registers `chord` and returns its stable id, or kInvalidBindingId when the key is 0, the
    // callback is null, or the table is full. `repeat` opts into auto-repeat firing;
    // `allow_superset` enables the broad modifier form above.
    BindingId add(KeyChord chord, AcceleratorCallback callback, void* user, bool repeat = false,
                  bool allow_superset = false);
    bool remove(BindingId id);
    [[nodiscard]] bool contains(BindingId id) const;
    [[nodiscard]] std::size_t size() const {
        return count_;
    }
    void clear();

    // Fires the matching binding for `event`, if any, and returns its id (0 when nothing fired).
    // The caller is responsible for the consumed/priority rule: the router invokes this only after
    // the focused node's handler ran and only when the event was not consumed.
    BindingId dispatch(const InputEvent& event);

private:
    struct Entry {
        BindingId id = 0;
        KeyChord chord{};
        AcceleratorCallback callback = nullptr;
        void* user = nullptr;
        bool repeat = false;
        bool allow_superset = false;
        bool active = false;
    };

    [[nodiscard]] Entry* find(BindingId id);

    std::array<Entry, kMaxBindings> entries_{};
    BindingId next_id_ = 1;
    std::size_t count_ = 0;
};

} // namespace tessera::ui
