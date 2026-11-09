// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/gesture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tessera::ui {

// Built-in deterministic recognizers. Each one tracks a single gesture (PinchRotate tracks one
// two-pointer interaction) and emits through the arena sink. Time always comes from the event or
// tick timestamp in seconds.

// Tap (and double-tap): claims on release when the press stayed within `tap_slop` and ended before
// `long_press_ms`. A second tap within `double_tap_ms` and `tap_slop` of the previous tap emits
// `DoubleTap` instead of a second `Tap`. Rejects once the press leaves the slop so a drag can win.
class TapRecognizer final : public GestureRecognizer {
public:
    [[nodiscard]] GestureKind kind() const override {
        return GestureKind::Tap;
    }

    RecognizerAction on_down(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_move(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_up(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_tick(double timestamp_s, GestureContext context) override;
    void on_cancel(const InputEvent& event, GestureContext context) override;
    void on_claim_lost(std::uint32_t pointer_id) override;

private:
    bool pressed_ = false;
    std::uint32_t pointer_id_ = 0;
    float down_x_ = 0.0f;
    float down_y_ = 0.0f;
    double down_time_ = 0.0;
    bool has_last_tap_ = false;
    std::uint32_t last_tap_pointer_id_ = 0;
    float last_tap_x_ = 0.0f;
    float last_tap_y_ = 0.0f;
    double last_tap_time_ = 0.0;
};

// Long press: emits `Began` once the press has been held for `long_press_ms` (driven by `tick`, or
// by a move/up that observes the elapsed time) and `Ended` on release. Rejects when the press
// leaves `tap_slop`; `Canceled` is emitted only after `Began`.
class LongPressRecognizer final : public GestureRecognizer {
public:
    [[nodiscard]] GestureKind kind() const override {
        return GestureKind::LongPress;
    }

    RecognizerAction on_down(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_move(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_up(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_tick(double timestamp_s, GestureContext context) override;
    void on_cancel(const InputEvent& event, GestureContext context) override;
    void on_claim_lost(std::uint32_t pointer_id) override;

private:
    bool pressed_ = false;
    bool began_ = false;
    std::uint32_t pointer_id_ = 0;
    float down_x_ = 0.0f;
    float down_y_ = 0.0f;
    float last_x_ = 0.0f;
    float last_y_ = 0.0f;
    double down_time_ = 0.0;
};

// Drag: claims when the press moves beyond `drag_threshold`, then emits `Began`, `Changed` per
// move, and `Ended` on release. `dx`/`dy` are the total displacement from the press point.
class DragRecognizer final : public GestureRecognizer {
public:
    [[nodiscard]] GestureKind kind() const override {
        return GestureKind::Drag;
    }

    RecognizerAction on_down(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_move(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_up(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_tick(double timestamp_s, GestureContext context) override;
    void on_cancel(const InputEvent& event, GestureContext context) override;
    void on_claim_lost(std::uint32_t pointer_id) override;

private:
    bool active_ = false;
    bool began_ = false;
    std::uint32_t pointer_id_ = 0;
    float down_x_ = 0.0f;
    float down_y_ = 0.0f;
    float last_x_ = 0.0f;
    float last_y_ = 0.0f;
};

// Swipe: a fast drag. Claims when the press has both passed `drag_threshold` and reached a
// deterministic velocity window (see the constants in the implementation): `Began`, then
// `Changed` per move, then `Ended` with the total `dx`/`dy` (direction is the sign). Registered
// alongside Drag, whichever is polled first claims; use one or the other for a given arena.
class SwipeRecognizer final : public GestureRecognizer {
public:
    [[nodiscard]] GestureKind kind() const override {
        return GestureKind::Swipe;
    }

    RecognizerAction on_down(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_move(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_up(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_tick(double timestamp_s, GestureContext context) override;
    void on_cancel(const InputEvent& event, GestureContext context) override;
    void on_claim_lost(std::uint32_t pointer_id) override;

private:
    struct Sample {
        double timestamp_s = 0.0;
        float x = 0.0f;
        float y = 0.0f;
    };

    void push_sample(const InputEvent& event);
    [[nodiscard]] float speed() const;
    void reset();

    bool active_ = false;
    bool began_ = false;
    std::uint32_t pointer_id_ = 0;
    float down_x_ = 0.0f;
    float down_y_ = 0.0f;
    std::array<Sample, 8> history_{};
    std::size_t history_count_ = 0;
};

// Pinch + rotate: two-pointer recognizer. Tracks the first two distinct pointer ids, claims once
// the pointer distance or angle changes beyond the thresholds, and emits `Pinch` and `Rotate`
// events (Began/Changed/Ended/Canceled). `scale` is current distance / initial distance and
// `rotation` is the wrapped radian angle delta; `pointer_id`/`secondary_pointer_id` are the two
// ids and `x`/`y` are their midpoint.
class PinchRotateRecognizer final : public GestureRecognizer {
public:
    [[nodiscard]] GestureKind kind() const override {
        return GestureKind::Pinch;
    }
    [[nodiscard]] bool multi_pointer() const override {
        return true;
    }

    RecognizerAction on_down(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_move(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_up(const InputEvent& event, GestureContext context) override;
    RecognizerAction on_tick(double timestamp_s, GestureContext context) override;
    void on_cancel(const InputEvent& event, GestureContext context) override;
    void on_claim_lost(std::uint32_t pointer_id) override;

private:
    bool has_first_ = false;
    bool has_second_ = false;
    bool began_ = false;
    std::uint32_t first_id_ = 0;
    std::uint32_t second_id_ = 0;
    float first_x_ = 0.0f;
    float first_y_ = 0.0f;
    float second_x_ = 0.0f;
    float second_y_ = 0.0f;
    float base_distance_ = 0.0f;
    float base_angle_ = 0.0f;
};

} // namespace tessera::ui
