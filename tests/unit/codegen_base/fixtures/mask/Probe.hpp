// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Probe` fixture for the ADR-056 dirty-mask tests. The component has
// no `bind::` members, so the class only befriends its generated base. Never regenerated.

#pragma once

#include "Probe.g.hpp"

namespace demo {

class Probe final : public gen::ProbeBase<Probe> {
private:
    friend struct gen::ProbeBase<Probe>;
};

} // namespace demo
