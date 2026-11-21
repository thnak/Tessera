// SPDX-License-Identifier: MIT

#include "tessera/ui/responsive/environment_set.hpp"

#include <algorithm>

namespace tessera::ui::responsive {

void EnvironmentSet::reserve(std::size_t consumers) {
    consumers_.reserve(consumers);
}

void EnvironmentSet::attach(const EnvironmentConsumer& consumer) {
    for (const EnvironmentConsumer& existing : consumers_) {
        if (existing.context == consumer.context && existing.provider == consumer.provider) {
            return; // idempotent: a rebuild that re-attaches the same component is a no-op
        }
    }
    consumers_.push_back(consumer);
}

void EnvironmentSet::detach(const EnvironmentConsumer& consumer) {
    consumers_.erase(std::remove_if(consumers_.begin(), consumers_.end(),
                                    [&](const EnvironmentConsumer& existing) {
                                        return existing.context == consumer.context &&
                                               existing.provider == consumer.provider;
                                    }),
                     consumers_.end());
}

std::size_t EnvironmentSet::push(std::string_view provider, std::string_view value) {
    std::size_t pushed = 0;
    for (const EnvironmentConsumer& consumer : consumers_) {
        if (consumer.provider != provider || consumer.set == nullptr) {
            continue;
        }
        consumer.set(consumer.context, value);
        ++pushed;
    }
    return pushed;
}

std::size_t EnvironmentSet::size_of(std::string_view provider) const {
    return static_cast<std::size_t>(
        std::count_if(consumers_.begin(), consumers_.end(),
                      [&](const EnvironmentConsumer& c) { return c.provider == provider; }));
}

} // namespace tessera::ui::responsive
