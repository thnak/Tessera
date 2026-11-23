// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `FormPage` record fixture for issue #834. The component has no
// `bind::` members, so the class only befriends its generated base. Never regenerated.

#pragma once

#include "FormPage.g.hpp"

namespace demo {

class FormPage final : public gen::FormPageBase<FormPage> {
private:
    friend struct gen::FormPageBase<FormPage>;
};

} // namespace demo
