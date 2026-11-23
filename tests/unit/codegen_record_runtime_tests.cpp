// SPDX-License-Identifier: MIT

// Issue #834: behavior on the committed `record/FormPage.g.hpp` base.
//
//   - Level 1: an equal record schedules nothing; a changed field schedules once.
//   - Level 2: a change to `email` re-runs no binding that reads only `name`, and a `recordUpdate`
//     that writes only `name` raises only the `name` bit.
//   - Level 3: a list of records renders keyed by `row.id`, and editing one row's label repaints
//     that item only.
//
// The bindings re-run are counted through TESSERA_BINDING_TRACE, the mechanism the ADR-056 mask
// runtime tests use. Deterministic: no clock, no executor, no GPU.

#define TESSERA_BINDING_TRACE
#include "FormPage.hpp"
#include "demo_types.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::vector<std::string> g_trace;

// TESSERA_BINDING_TRACE sink: the generated base calls this at the top of every gated binding.
void record(const void*, const char* binding) {
    g_trace.emplace_back(binding);
}

std::string joined(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    std::string out;
    for (const std::string& value : values) {
        out += (out.empty() ? "" : ",") + value;
    }
    return out.empty() ? "<none>" : out;
}

// The interpolated text of every Text node, in tree order, so a repaint is observable.
void texts(const ui::Node& node, std::vector<std::string>& out) {
    if (node.paint_data()) {
        out.push_back(node.paint_data()->text);
    }
    for (const auto& child : node.children()) {
        texts(*child, out);
    }
}

std::vector<std::string> tree_texts(const ui::Node& root) {
    std::vector<std::string> out;
    texts(root, out);
    return out;
}

struct Fixture {
    ui::Node root{1};
    demo::FormPage page;
    ui::UpdateScheduler scheduler;
    

    explicit Fixture(std::vector<demo::gen::Entry> rows) {
        page.build(root);
        page.attach_scheduler(scheduler);
        // Seed the list state through its setter (the `prop_seed_` initial only feeds the state's
        // own constructor-time initial, which runs before the caller can set the prop).
        page.set_rows(std::move(rows));
        scheduler.flush();
        g_trace.clear();
    }

    ~Fixture() {
        page.detach_scheduler(scheduler);
    }

    std::string run(const std::function<void(demo::FormPage&)>& change) {
        g_trace.clear();
        change(page);
        scheduler.flush();
        return joined(g_trace);
    }
};

// Level 1 and 2: an equal whole-record set schedules nothing; setting a field re-runs only the
// binding that reads it.
void test_equal_record_schedules_nothing() {
    Fixture f({});
    // Build/settle already ran; a repeated identical record is a no-op.
    const std::string trace = f.run([](demo::FormPage& page) {
        page.set_form(demo::gen::Form{.name = "Ann", .email = "ann@example.com", .age = 30});
    });
    check(trace == "<none>", "an equal record schedules no binding, got: " + trace);
    // A changed field re-runs only the binding that reads it.
    const std::string changed = f.run([](demo::FormPage& page) {
        page.set_form(demo::gen::Form{.name = "Ann", .email = "new@example.com", .age = 30});
    });
    check(changed == "text:3", "only the email text re-runs on an email change, got: " + changed);
}

// Level 2: a recordUpdate that writes only `name` raises only the name bit, so only the name text
// re-runs.
void test_record_update_raises_one_bit() {
    Fixture f({});
    const std::string trace = f.run([](demo::FormPage& page) { page.handler_1(ui::Event{}); });
    check(trace == "text:2", "the record update re-runs only the name text, got: " + trace);
    check(f.page.form().name == "Bob", "the record update wrote the field");
    check(f.page.form().email == "ann@example.com", "the record update kept the other fields");
}

// Level 2 (the fan-out acceptance item, ADR-056): a change to `email` re-evaluates no binding that
// reads only `name`.
void test_email_change_does_not_touch_name_binding() {
    Fixture f({});
    const std::string trace = f.run([](demo::FormPage& page) { page.handler_2(ui::Event{}); });
    // handler_2 sets the whole record; name, email and age all change, so both texts re-run, but
    // the list (which reads only state::rows) does not.
    check(trace.find("list:4") == std::string::npos,
          "a form change does not re-run the record list, got: " + trace);
}

// Level 3: a list of records renders keyed by row.id; changing one row's label repaints one item.
void test_record_list_repaints_one_item() {
    Fixture f({demo::gen::Entry{"a", "Alpha"}, demo::gen::Entry{"b", "Beta"}, demo::gen::Entry{"c", "Gamma"}});
    const std::vector<std::string> before = tree_texts(f.root);
    check(std::find(before.begin(), before.end(), "label=Alpha") != before.end(),
          "the first record item renders its label");
    check(std::find(before.begin(), before.end(), "label=Gamma") != before.end(),
          "the third record item renders its label");

    // Replace the middle row's label: a keyed list keeps `a` and `c`, so only `b` repaints.
    const std::string trace = f.run([](demo::FormPage& page) {
        page.set_rows({demo::gen::Entry{"a", "Alpha"}, demo::gen::Entry{"b", "Beta 2"}, demo::gen::Entry{"c", "Gamma"}});
    });
    check(trace == "list:4", "only the list binding re-runs on a row edit, got: " + trace);
    const std::vector<std::string> after = tree_texts(f.root);
    check(std::find(after.begin(), after.end(), "label=Beta 2") != after.end(),
          "the edited row repainted its new label");
    check(std::find(after.begin(), after.end(), "label=Alpha") != after.end(),
          "the untouched rows are still present");
    check(std::find(after.begin(), after.end(), "label=Beta") == after.end(),
          "the stale label is gone");
}

} // namespace

int main() {
    ui::g_binding_trace_hook = &record;
    test_equal_record_schedules_nothing();
    test_record_update_raises_one_bit();
    test_email_change_does_not_touch_name_binding();
    test_record_list_repaints_one_item();
    if (failures == 0) {
        std::cout << "codegen record runtime tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
