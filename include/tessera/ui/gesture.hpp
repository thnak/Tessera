// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tessera::ui {

// Gesture recognition model (spec: docs/specs/scroll-and-gestures-v1.md section 5).
//
// The arena and recognizers are pure state machines: they consume normalized `InputEvent`s and
// emit `GestureEvent`s through a bounded sink. They never read the wall clock (time comes from
// event and tick timestamps), never touch the scene graph, and allocate nothing on the event path.
enum class GestureKind : std::uint8_t {
    Tap,
    DoubleTap,
    LongPress,
    Drag,
    Swipe,
    Pinch,
    Rotate,
};

enum class GesturePhase : std::uint8_t {
    Began,
    Changed,
    Ended,
    Canceled,
};

struct GestureConfig {
    // Maximum press displacement for a tap, in logical pixels.
    float tap_slop = 8.0f;
    // Hold duration that turns a press into a long press, in milliseconds.
    float long_press_ms = 500.0f;
    // Movement that starts a drag/swipe/pinch, in logical pixels.
    float drag_threshold = 8.0f;
    // Maximum gap between two taps for a double tap, in milliseconds.
    float double_tap_ms = 300.0f;
    // When true, a second pointer may join and two-pointer recognizers can claim both ids. When
    // false, pointer downs while another pointer is active are ignored (single-pointer arena).
    bool simultaneous = true;
};

struct GestureEvent {
    GestureKind kind = GestureKind::Tap;
    GesturePhase phase = GesturePhase::Ended;
    std::uint32_t pointer_id = 0;
    // Second pointer for two-pointer gestures (Pinch/Rotate); 0 for single-pointer gestures.
    std::uint32_t secondary_pointer_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    float scale = 1.0f;
    float rotation = 0.0f;
    double timestamp_s = 0.0;
};

// Bounded output sink. Events beyond capacity are dropped; the arena never allocates.
class GestureSink {
public:
    static constexpr std::size_t kCapacity = 32;

    void emit(const GestureEvent& event);
    void clear();
    [[nodiscard]] std::size_t size() const {
        return count_;
    }
    [[nodiscard]] std::span<const GestureEvent> events() const {
        return {events_.data(), count_};
    }

private:
    std::array<GestureEvent, kCapacity> events_{};
    std::size_t count_ = 0;
};

enum class RecognizerAction : std::uint8_t {
    Ignore, // nothing to decide yet; the arena keeps polling other recognizers
    Claim,  // become the winner for this pointer (every active pointer when multi_pointer())
    Reject, // cannot handle this pointer; the arena stops polling this recognizer for it
};

struct GestureContext {
    const GestureConfig& config;
    GestureSink& sink;
};

// One recognizer instance participates in the arena. Handlers run in registration order until one
// claims; after a claim only the winner receives move/up/cancel for the claimed pointer(s).
// Recognizers hold their own per-gesture state and no global pointer state.
class GestureRecognizer {
public:
    virtual ~GestureRecognizer() = default;

    [[nodiscard]] virtual GestureKind kind() const = 0;
    // Two-pointer recognizers claim every active pointer at once (pinch/rotate).
    [[nodiscard]] virtual bool multi_pointer() const {
        return false;
    }

    virtual RecognizerAction on_down(const InputEvent& event, GestureContext context) = 0;
    virtual RecognizerAction on_move(const InputEvent& event, GestureContext context) = 0;
    virtual RecognizerAction on_up(const InputEvent& event, GestureContext context) = 0;
    // Called for active, unclaimed pointers (for example a long-press duration check). Time is
    // supplied by the caller from the simulated tick clock, never read from the wall clock.
    virtual RecognizerAction on_tick(double timestamp_s, GestureContext context) = 0;
    // Called only on the winner when its pointer is canceled (PointerCancel or window blur/focus).
    // A recognizer that emitted `Began` must emit `Canceled` here.
    virtual void on_cancel(const InputEvent& event, GestureContext context) = 0;
    // Called when another recognizer claims a pointer this one was still tracking, and when an
    // unclaimed pointer is canceled: drop per-pointer state for `pointer_id` without emitting
    // events. The arena "cancels" losers through this hook.
    virtual void on_claim_lost(std::uint32_t /*pointer_id*/) {}
};

// Deterministic arena: bounded recognizer list, bounded pointer slots, bounded event sink.
//
// Lifecycle per pointer: PointerDown registers a slot and polls the recognizers in order;
// recognizers may claim on down/move/tick. Once claimed, only the winner (plus every active
// pointer for a two-pointer recognizer) receives move/up/cancel. PointerUp/`PointerCancel` end the
// gesture; `PointerCancel` is first-class and a winner that already emitted `Began` must emit
// `Canceled` (the built-in recognizers do). A press canceled before any gesture began emits no
// gesture event. Window `Focus`/`Blur` input cancels every active pointer.
class GestureArena {
public:
    static constexpr std::size_t kMaxRecognizers = 8;
    static constexpr std::size_t kMaxPointers = 4;
    static constexpr std::size_t kMaxEvents = GestureSink::kCapacity;

    explicit GestureArena(const GestureConfig& config = {});

    GestureArena(const GestureArena&) = delete;
    GestureArena& operator=(const GestureArena&) = delete;

    // Registers a recognizer (not owned; it must outlive the arena). Returns false when the fixed
    // capacity is full.
    bool add(GestureRecognizer& recognizer);
    // Unregisters every recognizer and drops pointer/claim state without emitting events.
    void clear();

    // Feeds one normalized input event. Only pointer events are handled explicitly; Focus/Blur
    // cancel every active pointer. The event sink is cleared first, so `events()` logs this call.
    void process(const InputEvent& event);
    // Advances time-driven recognizers with simulated time in seconds. Clears the sink first.
    void tick(double timestamp_s);

    [[nodiscard]] std::span<const GestureEvent> events() const {
        return sink_.events();
    }
    [[nodiscard]] std::size_t event_count() const {
        return sink_.size();
    }
    [[nodiscard]] const GestureConfig& config() const {
        return config_;
    }
    void set_config(const GestureConfig& config) {
        config_ = config;
    }

    // True while the arena tracks a down pointer; true when a recognizer has won that pointer.
    [[nodiscard]] bool tracking(std::uint32_t pointer_id) const;
    [[nodiscard]] bool claimed(std::uint32_t pointer_id) const;
    [[nodiscard]] std::size_t active_pointer_count() const;

private:
    struct Slot {
        std::uint32_t pointer_id = 0;
        bool active = false;
        bool claimed = false;
        GestureRecognizer* winner = nullptr;
        std::uint16_t rejected = 0;
    };

    [[nodiscard]] std::size_t find_slot(std::uint32_t pointer_id) const;
    [[nodiscard]] std::size_t free_slot() const;
    [[nodiscard]] GestureContext context();
    using Handler = RecognizerAction (GestureRecognizer::*)(const InputEvent&, GestureContext);
    void poll(const InputEvent& event, Handler handler, std::size_t slot_index);
    void poll_tick(double timestamp_s, std::size_t slot_index);
    void claim(GestureRecognizer& recognizer, std::size_t slot_index);
    void notify_claim_lost(std::size_t slot_index, const GestureRecognizer* winner);
    void release(std::size_t slot_index);
    void release_all_for(const GestureRecognizer& recognizer);
    void cancel_all(double timestamp_s);
    void handle_down(const InputEvent& event);
    void handle_up(const InputEvent& event, std::size_t slot_index);

    GestureConfig config_{};
    std::array<GestureRecognizer*, kMaxRecognizers> recognizers_{};
    std::size_t recognizer_count_ = 0;
    std::array<Slot, kMaxPointers> slots_{};
    GestureSink sink_{};
};

} // namespace tessera::ui
