// SPDX-License-Identifier: MIT
//
// Developer-owned half of the ADR-040 fixture: derives from the generated `SampleBase<Self>` and
// defines every `bind::` member. Never regenerated (the generator only writes Sample.g.hpp).

#pragma once

#include "Sample.g.hpp"

namespace demo {

class Sample final : public gen::SampleBase<Sample> {
public:
    int clicks = 0;
    int syncs = 0;
    int cleanups = 0;
    int last_count_seen = -1;

    // Public so the test can call them directly and through the generated thunks.
    void on_click(const tessera::ui::ClickEvent& event);
    void sync();
    void sync_cleanup();

private:
    friend struct gen::SampleBase<Sample>;
};

} // namespace demo
