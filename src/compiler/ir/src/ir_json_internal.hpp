// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/json_value.hpp"

#include <vector>

namespace tessera::compiler::ir {

JsonValue actions_to_json(const std::vector<Action>& actions);
JsonValue params_to_json(const std::vector<Param>& params);

} // namespace tessera::compiler::ir
