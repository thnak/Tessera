// SPDX-License-Identifier: MIT
//
// Developer-owned class of the forms fixture (never regenerated).

#pragma once

#include "FieldPage.g.hpp"

namespace demo {

class FieldPage final : public gen::FieldPageBase<FieldPage> {
public:
    void on_validate(const tessera::ui::TextChangeEvent&) {}

private:
    friend struct gen::FieldPageBase<FieldPage>;
};

} // namespace demo
