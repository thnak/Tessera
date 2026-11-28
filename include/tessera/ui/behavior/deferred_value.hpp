// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>

// `deferred_value` (ADR-067 item 5, issue #656): the engine behavior behind `useDeferredValue`.
// Its one output is the settled value of a scalar.
//
// Options, in order: `value` (the scalar being deferred: a bool, a number or a string token) and
// `hold` (true while a transition of the component has a pending read). While `hold` is true the
// output keeps the last settled value; otherwise it is `value`, which then becomes the settled
// value. The output has the kind of the option, one stored scalar, so an update allocates nothing.
// The generated base runs the same rule inline over the component's own transition mark.
namespace tessera::ui::behavior {

class DeferredValue final : public Behavior {
public:
    static constexpr std::size_t kOutputCount = 1;

    DeferredValue() = default;

    void attach(const Targets& targets, const Services& services, const ArgsView& args) override;
    void update(const ArgsView& args) override;
    void detach() override;
    [[nodiscard]] std::span<const OutputValue> outputs() const override;

private:
    void apply(const ArgsView& args);

    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
