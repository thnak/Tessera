// SPDX-License-Identifier: MIT
//
// Developer-owned class of the forms fixture (never regenerated).

#pragma once

#include "FormPage.g.hpp"

namespace demo {

class FormPage final : public gen::FormPageBase<FormPage> {
public:
    void on_validate_form(const tessera::ui::Event&) {}
    void on_submit(const tessera::ui::Event&) {}

private:
    friend struct gen::FormPageBase<FormPage>;
};

} // namespace demo
