// SPDX-License-Identifier: MIT

// Issue #440: how many `update()` calls does one state change cost on generated components?
//
// Measured on the committed `instances/{Page,Row}.g.hpp` (the static `Row` child and the keyed list
// of per-item `Row` instances), with a counting developer class for `Row`
// (`fixtures/update_count/Row.hpp`). Two numbers are recorded per scenario:
//
//   - `ticket`: `UpdateFlushStats::updated`, the updates the scheduler ran from a queued ticket;
//   - `direct`: `Row::update()` calls made by an owner's generated `update()` (the fan-out).
//
// A Row therefore runs `direct + (ticket - page tickets)` times in one flush. Deterministic: no
// clock, no executor, no GPU. The asserted numbers are the *measured* behavior, written down so a
// later change to the fan-out shows up as a failing, reviewable diff and not as a silent cost.
//
// ADR-056 (issues #442, #443) rewrote the expected numbers: an owner never calls a child's
// `update()` (`direct` is 0 in every scenario) and schedules a child only when it wrote one of the
// child's props, so an owner change that feeds no child runs one ticket whatever `n` is.

#include "Page.hpp"
#include "Row.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
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

// What one flush cost.
struct Measure {
    std::uint32_t ticket = 0; // updates the scheduler ran from tickets
    std::uint32_t passes = 0;
    int direct = 0; // Row::update() calls from an owner's generated update()
};

// A page with `n` per-item Row instances (one per entry key), attached to a scheduler and settled,
// so the next flush measures only what the scenario changes.
struct Fixture {
    ui::Node root{1000};
    demo::Page page;
    ui::UpdateScheduler scheduler;

    explicit Fixture(std::size_t n) {
        page.build(root);
        std::vector<std::string> entries;
        entries.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            entries.push_back("e" + std::to_string(i));
        }
        // Detached on purpose: the items exist before the scheduler sees them, so the measured
        // flush is not polluted by late attachment.
        page.set_entries(std::move(entries));
        page.update();
        page.attach_scheduler(scheduler);
        scheduler.flush();
        demo::g_row_direct_updates = 0;
    }

    ~Fixture() {
        page.detach_scheduler(scheduler);
    }

    Measure flush() {
        demo::g_row_direct_updates = 0;
        const ui::UpdateFlushStats stats = scheduler.flush();
        return Measure{stats.updated, stats.passes, demo::g_row_direct_updates};
    }
};

void report(std::string_view scenario, std::size_t n, const Measure& m) {
    std::cout << scenario << " n=" << n << ": ticket=" << m.ticket << " direct=" << m.direct
              << " passes=" << m.passes << "\n";
}

std::string tag(std::string_view scenario, std::size_t n) {
    return std::string(scenario) + " (n=" + std::to_string(n) + "): ";
}

// The static `Row` owns its state, and nothing else changed. The scheduler runs exactly that one
// ticket; the page and the N item rows are not touched.
void child_state_only(std::size_t n) {
    Fixture f(n);
    f.page.child_5().set_count(7);
    const Measure m = f.flush();
    report("child-state-only", n, m);
    check(m.ticket == 1, tag("child-state-only", n) + "one ticket ran");
    check(m.direct == 0, tag("child-state-only", n) + "no owner fan-out");
    check(m.passes == 1, tag("child-state-only", n) + "one pass");
}

// The owner's own state changes (`nums` feeds no Row at all). Only the page's ticket runs: it wrote
// no child prop, so it scheduled no child and called no child's `update()` (was n + 1 calls).
void parent_state_feeds_no_child(std::size_t n) {
    Fixture f(n);
    f.page.set_nums(std::vector<int>{1, 2, 3});
    const Measure m = f.flush();
    report("parent-state", n, m);
    check(m.ticket == 1, tag("parent-state", n) + "one ticket ran (the page)");
    check(m.direct == 0, tag("parent-state", n) + "the page called no child update()");
    check(m.passes == 1, tag("parent-state", n) + "one pass");
}

// The owner and its static child are both dirty in one flush: each runs exactly once, in one pass
// (was n + 2 Row runs: the page's direct call plus the child's own ticket).
void parent_and_child_dirty(std::size_t n) {
    Fixture f(n);
    f.page.set_nums(std::vector<int>{1, 2, 3});
    f.page.child_5().set_count(7);
    const Measure m = f.flush();
    report("parent+child", n, m);
    check(m.ticket == 2, tag("parent+child", n) + "two tickets ran (page and the static Row)");
    check(m.direct == 0, tag("parent+child", n) + "the page called no child update()");
    check(m.passes == 1, tag("parent+child", n) + "still one pass");
}

std::vector<std::string> entry_names(std::size_t n) {
    std::vector<std::string> entries;
    entries.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        entries.push_back("e" + std::to_string(i));
    }
    return entries;
}

// A parent change that does feed children: one entry appended. Only the page and the new item Row
// run (a created instance is scheduled once); the n existing Rows' prop hashes are unchanged, so
// none of them is scheduled. Still one pass: the new Row is later in the same pass.
void parent_appends_one_entry(std::size_t n) {
    Fixture f(n);
    std::vector<std::string> entries = entry_names(n);
    entries.push_back("new");
    f.page.set_entries(std::move(entries));
    const Measure m = f.flush();
    report("append", n, m);
    check(m.ticket == 2, tag("append", n) + "the page and the one new Row ran");
    check(m.direct == 0, tag("append", n) + "no direct child update()");
    check(m.passes == 1, tag("append", n) + "the new Row ran in the same pass");
}

// A keyed reorder moves the item nodes with their instances: no key is new and no prop hash
// changed, so no Row is scheduled and the instance state follows the key.
void parent_reorders_entries(std::size_t n) {
    Fixture f(n);
    std::vector<std::string> entries = entry_names(n);
    std::swap(entries.front(), entries.back());
    f.page.set_entries(std::move(entries));
    const Measure m = f.flush();
    report("reorder", n, m);
    check(m.ticket == 1, tag("reorder", n) + "only the page ran; no Row was scheduled");
    check(m.passes == 1, tag("reorder", n) + "one pass");
}

} // namespace

int main() {
    for (const std::size_t n : {std::size_t{10}, std::size_t{100}, std::size_t{1000}}) {
        child_state_only(n);
        parent_state_feeds_no_child(n);
        parent_and_child_dirty(n);
        parent_appends_one_entry(n);
        parent_reorders_entries(n);
    }
    if (failures == 0) {
        std::cout << "codegen_update_fanout: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
