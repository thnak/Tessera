// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>

// `transition` (ADR-067 item 4, issue #656): the engine behavior behind `useTransition`. Its one
// output is `isPending`.
//
// The pending state itself is the component's `TransitionMark`
// (`tessera/ui/pending/transition.hpp`), which the marked setter and the resource subscription
// drive, so this behavior owns no state of its own: it republishes the one bool option `pending` as
// its `isPending` output. A host that drives the behavior by hand (the preview) passes the flag;
// the generated base does not attach it and reads the mark directly, which is why this class holds
// no scene node and no target.
namespace tessera::ui::behavior {

class Transition final : public Behavior {
public:
    // The manifest's output order: `isPending`.
    static constexpr std::size_t kOutputCount = 1;

    Transition() = default;

    void attach(const Targets& targets, const Services& services, const ArgsView& args) override;
    void update(const ArgsView& args) override;
    void detach() override;
    [[nodiscard]] std::span<const OutputValue> outputs() const override;

private:
    void apply(const ArgsView& args);

    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
