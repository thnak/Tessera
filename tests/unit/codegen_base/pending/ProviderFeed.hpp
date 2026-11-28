// SPDX-License-Identifier: MIT

#pragma once

#include "ProviderFeed.g.hpp"

// The developer class of the provider-bound pending fixture (#656 follow-ups): it only counts how
// often the effect over `row.status` ran.
namespace pending {

class ProviderFeed final : public gen::ProviderFeedBase<ProviderFeed> {
public:
    void row() {
        ++effect_runs_;
    }
    [[nodiscard]] int effect_runs() const {
        return effect_runs_;
    }

private:
    int effect_runs_ = 0;
};

} // namespace pending
