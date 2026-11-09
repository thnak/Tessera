// SPDX-License-Identifier: MIT

#include "tessera/ui/input/key_bindings.hpp"

namespace tessera::ui {

Modifier normalize_chord_modifiers(Modifier modifiers) {
    constexpr auto kChordMask = static_cast<std::uint8_t>(Modifier::Shift | Modifier::Control |
                                                          Modifier::Alt | Modifier::Meta);
    return static_cast<Modifier>(static_cast<std::uint8_t>(modifiers) & kChordMask);
}

KeyChord normalize_chord(KeyChord chord) {
    chord.modifiers = normalize_chord_modifiers(chord.modifiers);
    return chord;
}

KeyBindingRegistry::Entry* KeyBindingRegistry::find(BindingId id) {
    if (id == kInvalidBindingId) {
        return nullptr;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].id == id) {
            return &entries_[i];
        }
    }
    return nullptr;
}

BindingId KeyBindingRegistry::add(KeyChord chord, AcceleratorCallback callback, void* user,
                                  bool repeat, bool allow_superset) {
    if (chord.key == 0 || callback == nullptr || count_ == kMaxBindings) {
        return kInvalidBindingId;
    }
    Entry& entry = entries_[count_++];
    entry.id = next_id_++;
    entry.chord = normalize_chord(chord);
    entry.callback = callback;
    entry.user = user;
    entry.repeat = repeat;
    entry.allow_superset = allow_superset;
    entry.active = true;
    // Wrapping would make an old id ambiguous; reset to 1 only after a full 32-bit cycle.
    if (next_id_ == kInvalidBindingId) {
        next_id_ = 1;
    }
    return entry.id;
}

bool KeyBindingRegistry::remove(BindingId id) {
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].id != id) {
            continue;
        }
        for (std::size_t j = i + 1; j < count_; ++j) {
            entries_[j - 1] = entries_[j];
        }
        --count_;
        entries_[count_] = Entry{};
        return true;
    }
    return false;
}

bool KeyBindingRegistry::contains(BindingId id) const {
    if (id == kInvalidBindingId) {
        return false;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].id == id) {
            return true;
        }
    }
    return false;
}

void KeyBindingRegistry::clear() {
    entries_ = {};
    count_ = 0;
}

BindingId KeyBindingRegistry::dispatch(const InputEvent& event) {
    if (event.type != InputEventType::KeyDown || event.key == 0) {
        return kInvalidBindingId;
    }
    const Modifier event_modifiers = normalize_chord_modifiers(event.modifiers);

    // Snapshot matching ids first (bounded), so callbacks may mutate the registry safely, then fire
    // the first match that still exists. Exact matches precede broad matches; registration order
    // breaks ties within a class.
    std::array<BindingId, kMaxBindings> exact{};
    std::array<BindingId, kMaxBindings> broad{};
    std::size_t exact_count = 0;
    std::size_t broad_count = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        const Entry& entry = entries_[i];
        if (entry.chord.key != event.key || (event.repeat && !entry.repeat)) {
            continue;
        }
        if (!entry.allow_superset && entry.chord.modifiers == event_modifiers) {
            exact[exact_count++] = entry.id;
        } else if (entry.allow_superset &&
                   (event_modifiers & entry.chord.modifiers) == entry.chord.modifiers) {
            broad[broad_count++] = entry.id;
        }
    }

    const auto fire = [this, &event](const std::array<BindingId, kMaxBindings>& candidates,
                                     std::size_t candidate_count) -> BindingId {
        for (std::size_t i = 0; i < candidate_count; ++i) {
            Entry* entry = find(candidates[i]);
            if (entry == nullptr || !entry->active) {
                continue; // removed by an earlier callback in this dispatch
            }
            // Capture the id and callback before invoking: the callback may remove itself (which
            // shifts the entry table), so the entry must not be read afterwards.
            const BindingId id = entry->id;
            const AcceleratorCallback callback = entry->callback;
            void* const user = entry->user;
            callback(user, event);
            return id;
        }
        return kInvalidBindingId;
    };

    const BindingId fired = fire(exact, exact_count);
    if (fired != kInvalidBindingId) {
        return fired;
    }
    return fire(broad, broad_count);
}

} // namespace tessera::ui
