// SPDX-License-Identifier: MIT
//
// Developer-owned class of the compose fixture (never regenerated).

#pragma once

#include "CardPage.g.hpp"

namespace compose {

class CardPage final : public gen::CardPageBase<CardPage> {
public:
    void on_tap(const tessera::ui::Event&) {}

private:
    friend struct gen::CardPageBase<CardPage>;
};

} // namespace compose
