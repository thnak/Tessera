// SPDX-License-Identifier: MIT

// Issue #463: the inspector's reverse lookup from a scoped class to the authored name and file.

#include "ir_test_util.hpp"
#include "tessera/compiler/ir/authored_class.hpp"

#include <vector>

namespace {

using namespace ir_test;
namespace ir = tessera::compiler::ir;

std::vector<ir::StylesheetRef> two_modules() {
    ir::StylesheetRef card{"css::ui/Card.module", "ui/Card.module.css", "h1", {}};
    card.classes = {{"card", "card_0b82a5"}, {"title", "title_0b82a5"}};
    ir::StylesheetRef panel{"css::ui/Panel.module", "ui/Panel.module.css", "h2", {}};
    panel.classes = {{"title", "title_9c41d0"}};
    ir::StylesheetRef plain{"css::ui/app", "ui/app.css", "h3", {}};
    return {card, panel, plain};
}

// The same authored name in two files is two scoped names; each resolves to its own file.
void test_same_local_name_resolves_to_its_own_file() {
    const std::vector<ir::StylesheetRef> sheets = two_modules();
    const auto card = ir::find_authored_class(sheets, "title_0b82a5");
    const auto panel = ir::find_authored_class(sheets, "title_9c41d0");
    check(card.has_value() && panel.has_value(), "both scoped names resolve");
    if (card && panel) {
        check_eq(card->name, "title", "the authored name, not the hash");
        check_eq(card->file, "ui/Card.module.css", "the declaring file");
        check_eq(panel->name, "title", "the same authored name in the other file");
        check_eq(panel->file, "ui/Panel.module.css", "its own file");
        check(card->scoped && panel->scoped, "both are module classes");
    }
}

void test_unknown_and_empty_names_do_not_resolve() {
    const std::vector<ir::StylesheetRef> sheets = two_modules();
    check(!ir::find_authored_class(sheets, "title").has_value(),
          "an authored name is not a scoped name");
    check(!ir::find_authored_class(sheets, "").has_value(), "empty resolves to nothing");
    check(!ir::find_authored_class({}, "title_0b82a5").has_value(), "no sheets, no match");
}

// A node's className mixes scoped and global classes; the inspector keeps order and shows a global
// class as written.
void test_class_list_keeps_order_and_global_classes() {
    const std::vector<ir::StylesheetRef> sheets = two_modules();
    const auto list =
        ir::authored_classes(sheets, "  card_0b82a5  g-root   on_0000ff title_9c41d0 ");
    check_eq(std::to_string(list.size()), "4", "four tokens, extra spaces ignored");
    if (list.size() == 4) {
        check_eq(list[0].name, "card", "first is the authored name");
        check(list[0].scoped, "first is a module class");
        check_eq(list[1].name, "g-root", "a global class is shown as written");
        check(!list[1].scoped && list[1].file.empty(), "a global class has no file");
        check_eq(list[2].name, "on_0000ff", "a scoped-looking name no module maps stays verbatim");
        check_eq(list[3].file, "ui/Panel.module.css", "last resolves to the second module");
    }
    check(ir::authored_classes(sheets, "").empty(), "no classes, no entries");
}

} // namespace

int main() {
    test_same_local_name_resolves_to_its_own_file();
    test_unknown_and_empty_names_do_not_resolve();
    test_class_list_keeps_order_and_global_classes();
    return finish("ir authored class tests");
}
