// SPDX-License-Identifier: MIT

#pragma once

namespace tessera::runtime {

class Clock {
public:
    Clock() = default;
    virtual ~Clock() = default;

    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    [[nodiscard]] virtual double now_seconds() const = 0;
};

class ManualClock final : public Clock {
public:
    explicit ManualClock(double start_seconds = 0.0);

    [[nodiscard]] double now_seconds() const override;
    void advance(double seconds);

private:
    double now_ = 0.0;
};

} // namespace tessera::runtime
