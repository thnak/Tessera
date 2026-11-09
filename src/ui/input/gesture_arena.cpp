// SPDX-License-Identifier: MIT

#include "tessera/ui/gesture.hpp"

namespace tessera::ui {
namespace {

constexpr std::uint16_t recognizer_bit(std::size_t index) {
    return static_cast<std::uint16_t>(1u << index);
}

} // namespace

void GestureSink::emit(const GestureEvent& event) {
    // Bounded: a busy tick drops extra events instead of allocating.
    if (count_ == kCapacity) {
        return;
    }
    events_[count_++] = event;
}

void GestureSink::clear() {
    count_ = 0;
}

GestureArena::GestureArena(const GestureConfig& config)
    : config_(config) {}

bool GestureArena::add(GestureRecognizer& recognizer) {
    if (recognizer_count_ == kMaxRecognizers) {
        return false;
    }
    recognizers_[recognizer_count_++] = &recognizer;
    return true;
}

void GestureArena::clear() {
    recognizer_count_ = 0;
    slots_ = {};
    sink_.clear();
}

std::size_t GestureArena::find_slot(std::uint32_t pointer_id) const {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].active && slots_[i].pointer_id == pointer_id) {
            return i;
        }
    }
    return slots_.size();
}

std::size_t GestureArena::free_slot() const {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].active) {
            return i;
        }
    }
    return slots_.size();
}

std::size_t GestureArena::active_pointer_count() const {
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.active) {
            ++count;
        }
    }
    return count;
}

bool GestureArena::tracking(std::uint32_t pointer_id) const {
    return find_slot(pointer_id) != slots_.size();
}

bool GestureArena::claimed(std::uint32_t pointer_id) const {
    const std::size_t index = find_slot(pointer_id);
    return index != slots_.size() && slots_[index].claimed;
}

GestureContext GestureArena::context() {
    return GestureContext{config_, sink_};
}

void GestureArena::process(const InputEvent& event) {
    sink_.clear();
    switch (event.type) {
    case InputEventType::PointerDown:
        handle_down(event);
        return;
    case InputEventType::PointerMove: {
        const std::size_t index = find_slot(event.pointer_id);
        if (index == slots_.size()) {
            return;
        }
        if (slots_[index].claimed) {
            slots_[index].winner->on_move(event, context());
            return;
        }
        poll(event, &GestureRecognizer::on_move, index);
        return;
    }
    case InputEventType::PointerUp: {
        const std::size_t index = find_slot(event.pointer_id);
        if (index == slots_.size()) {
            return;
        }
        handle_up(event, index);
        return;
    }
    case InputEventType::PointerCancel: {
        const std::size_t index = find_slot(event.pointer_id);
        if (index == slots_.size()) {
            return;
        }
        if (slots_[index].claimed) {
            GestureRecognizer* winner = slots_[index].winner;
            winner->on_cancel(event, context());
            release_all_for(*winner);
        } else {
            // Nothing was public yet, but candidates may still hold per-pointer state.
            notify_claim_lost(index, nullptr);
            release(index);
        }
        return;
    }
    case InputEventType::Focus:
    case InputEventType::Blur:
        cancel_all(event.timestamp_s);
        return;
    default:
        return;
    }
}

void GestureArena::handle_down(const InputEvent& event) {
    // One slot per live pointer id; a duplicate down without an up is ignored.
    if (find_slot(event.pointer_id) != slots_.size()) {
        return;
    }
    // A second pointer only joins while `simultaneous` allows; otherwise the arena stays
    // single-pointer and the extra contact is ignored until the first one lifts.
    if (!config_.simultaneous && active_pointer_count() > 0) {
        return;
    }
    const std::size_t index = free_slot();
    if (index == slots_.size()) {
        return;
    }
    slots_[index] = Slot{};
    slots_[index].pointer_id = event.pointer_id;
    slots_[index].active = true;
    poll(event, &GestureRecognizer::on_down, index);
}

void GestureArena::handle_up(const InputEvent& event, std::size_t slot_index) {
    if (slots_[slot_index].claimed) {
        GestureRecognizer* winner = slots_[slot_index].winner;
        winner->on_up(event, context());
        release_all_for(*winner);
        return;
    }
    poll(event, &GestureRecognizer::on_up, slot_index);
    // Polling may have produced a claim (tap/double-tap/long-press); finalize it now.
    if (slots_[slot_index].claimed) {
        release_all_for(*slots_[slot_index].winner);
    } else {
        release(slot_index);
    }
}

void GestureArena::tick(double timestamp_s) {
    sink_.clear();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (!slots_[index].active || slots_[index].claimed) {
            continue;
        }
        poll_tick(timestamp_s, index);
    }
}

void GestureArena::poll(const InputEvent& event, Handler handler, std::size_t slot_index) {
    for (std::size_t i = 0; i < recognizer_count_; ++i) {
        if (slots_[slot_index].claimed) {
            return;
        }
        if ((slots_[slot_index].rejected & recognizer_bit(i)) != 0) {
            continue;
        }
        GestureRecognizer& recognizer = *recognizers_[i];
        const RecognizerAction action = (recognizer.*handler)(event, context());
        if (action == RecognizerAction::Claim) {
            claim(recognizer, slot_index);
            return;
        }
        if (action == RecognizerAction::Reject) {
            slots_[slot_index].rejected |= recognizer_bit(i);
        }
    }
}

void GestureArena::poll_tick(double timestamp_s, std::size_t slot_index) {
    for (std::size_t i = 0; i < recognizer_count_; ++i) {
        if (slots_[slot_index].claimed) {
            return;
        }
        if ((slots_[slot_index].rejected & recognizer_bit(i)) != 0) {
            continue;
        }
        GestureRecognizer& recognizer = *recognizers_[i];
        const RecognizerAction action = recognizer.on_tick(timestamp_s, context());
        if (action == RecognizerAction::Claim) {
            claim(recognizer, slot_index);
            return;
        }
        if (action == RecognizerAction::Reject) {
            slots_[slot_index].rejected |= recognizer_bit(i);
        }
    }
}

void GestureArena::claim(GestureRecognizer& recognizer, std::size_t slot_index) {
    // A two-pointer recognizer owns every active pointer; a single-pointer one owns only the
    // pointer that produced the claim. Losers are told to drop their per-pointer state.
    const bool all_pointers = recognizer.multi_pointer();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].active) {
            continue;
        }
        if (i == slot_index || all_pointers) {
            notify_claim_lost(i, &recognizer);
            slots_[i].claimed = true;
            slots_[i].winner = &recognizer;
        }
    }
}

void GestureArena::notify_claim_lost(std::size_t slot_index, const GestureRecognizer* winner) {
    const std::uint32_t pointer_id = slots_[slot_index].pointer_id;
    for (std::size_t i = 0; i < recognizer_count_; ++i) {
        if (recognizers_[i] == winner) {
            continue;
        }
        if ((slots_[slot_index].rejected & recognizer_bit(i)) != 0) {
            continue;
        }
        recognizers_[i]->on_claim_lost(pointer_id);
    }
}

void GestureArena::release(std::size_t slot_index) {
    slots_[slot_index] = Slot{};
}

void GestureArena::release_all_for(const GestureRecognizer& recognizer) {
    for (Slot& slot : slots_) {
        if (slot.winner == &recognizer) {
            slot = Slot{};
        }
    }
}

void GestureArena::cancel_all(double timestamp_s) {
    std::array<const GestureRecognizer*, kMaxPointers> canceled{};
    std::size_t canceled_count = 0;
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (!slots_[index].active) {
            continue;
        }
        if (slots_[index].claimed && slots_[index].winner != nullptr) {
            GestureRecognizer* winner = slots_[index].winner;
            bool seen = false;
            for (std::size_t i = 0; i < canceled_count; ++i) {
                if (canceled[i] == winner) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                InputEvent event;
                event.type = InputEventType::PointerCancel;
                event.pointer_id = slots_[index].pointer_id;
                event.timestamp_s = timestamp_s;
                winner->on_cancel(event, context());
                canceled[canceled_count++] = winner;
            }
        } else {
            notify_claim_lost(index, nullptr);
        }
        slots_[index] = Slot{};
    }
}

} // namespace tessera::ui
