// SPDX-License-Identifier: MIT
//
// StateCell helpers used by generated snapshot_state() (issue #7): each C++ type a state cell can
// have maps to the right scalar alternative, and lists keep their elements in order.

#include "tessera/ui/scene/state_snapshot.hpp"

#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {

namespace ui = tessera::ui;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "state_snapshot: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    const ui::StateCell flag = ui::state_cell("state::on", true);
    const ui::StateCell count = ui::state_cell("state::count", 7);
    const ui::StateCell ratio = ui::state_cell("anim::fade", 0.5);
    const ui::StateCell label = ui::state_cell("state::label", std::string("hi"));
    bool ok = expect(!flag.is_list && std::holds_alternative<bool>(flag.value) &&
                         std::get<bool>(flag.value),
                     "a bool cell keeps its type");
    ok = expect(std::holds_alternative<int>(count.value) && std::get<int>(count.value) == 7,
                "an int cell is not a bool or a double") &&
         ok;
    ok = expect(std::holds_alternative<double>(ratio.value) && std::get<double>(ratio.value) == 0.5,
                "a float cell is a double") &&
         ok;
    ok = expect(std::holds_alternative<std::string>(label.value) &&
                    std::get<std::string>(label.value) == "hi" && label.name == "state::label",
                "a string cell and its name") &&
         ok;

    const ui::StateCell ints = ui::state_cell("state::rows", std::vector<int>{3, 1, 2});
    ok = expect(ints.is_list && ints.items.size() == 3 && std::get<int>(ints.items[0]) == 3 &&
                    std::get<int>(ints.items[2]) == 2,
                "a list cell keeps its elements in order") &&
         ok;
    const ui::StateCell flags = ui::state_cell("state::checks", std::vector<bool>{true, false});
    ok = expect(flags.is_list && flags.items.size() == 2 && std::get<bool>(flags.items[0]) &&
                    !std::get<bool>(flags.items[1]),
                "a list of bools") &&
         ok;
    const ui::StateCell names = ui::state_cell("state::names", std::vector<std::string>{"a", "b"});
    ok = expect(names.items.size() == 2 && std::get<std::string>(names.items[1]) == "b",
                "a list of strings") &&
         ok;
    const ui::StateCell empty = ui::state_cell("state::none", std::vector<double>{});
    ok = expect(empty.is_list && empty.items.empty(), "an empty list") && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "state_snapshot: ok\n";
    return 0;
}
