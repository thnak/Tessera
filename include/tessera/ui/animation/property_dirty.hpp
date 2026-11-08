// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/dirty.hpp"
#include "tessera/ui/style/parser.hpp"

#include <string_view>
#include <vector>

namespace tessera::ui::animation {

bool is_layout_property(std::string_view property);
Dirty dirty_for_property(std::string_view property);
Dirty dirty_for_declarations(const std::vector<style::Declaration>& declarations);

} // namespace tessera::ui::animation
