// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Row` instance fixture (ADR-040): a component with a list-typed prop
// (`notes`) and one state cell (`count`). Never regenerated.

#pragma once

#include "Row.g.hpp"

namespace demo {

class Row final : public gen::RowBase<Row> {
public:
private:
    friend struct gen::RowBase<Row>;
};

} // namespace demo
