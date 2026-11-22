// SPDX-License-Identifier: MIT

// ADR-067 spike #661, runtime side: the generated component of `codegen_pending_module.hpp`
// compiled and driven through the fake resource. This is the half of the evidence the evaluator
// cannot give: the boundary really swaps scene subtrees, the old subtree really survives a refresh,
// and the update really allocates nothing.
//
// The allocation gate is the same shape as the virtual pool's (tests/unit/virtual): a replaced
// global `operator new` counts, and the count must not move across a full transition cycle. A
// wall-clock number cannot be a gate, so this is the assertion instead.

#include "Feed.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <string_view>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

} // namespace

// Allocation counting (the virtual pool's pattern): a replaced global operator new/delete pair.
namespace {
std::size_t g_allocations = 0;
} // namespace

void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* block = std::malloc(size)) {
        return block;
    }
    throw std::bad_alloc();
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

namespace {

// The text payload of a node, or an empty view when it has none.
std::string_view text_of(const ui::Node& node) {
    const std::optional<ui::PaintData>& paint = node.paint_data();
    if (!paint) {
        return {};
    }
    return paint->text;
}

// The first descendant (or self) whose text payload is `text`.
const ui::Node* find_text(const ui::Node& node, std::string_view text) {
    if (text_of(node) == text) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_text(*child, text)) {
            return found;
        }
    }
    return nullptr;
}

// The first descendant (or self) of `kind`.
const ui::Node* find_kind(const ui::Node& node, std::string_view kind) {
    if (node.element_kind() == kind) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_kind(*child, kind)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    pending::Feed feed;
    ui::Node root(1000);
    feed.build(root);

    // --- fallback while Loading with no data ------------------------------------------------
    // The boundary attached its content subtree at build time (the resource is Idle, so nothing
    // suspends yet).
    check(feed.pending_fallback_live() == false, "an idle resource shows the content");
    check(find_text(root, "Loading") == nullptr, "the fallback is not attached while idle");

    // A fetch with no data: the boundary must swap to the fallback.
    const std::uint64_t epoch = feed.pending_begin_fetch();
    check(epoch == 1, "the first fetch is epoch 1");
    feed.pending_sync();
    check(feed.pending_fallback_live(), "the boundary shows its fallback while loading");
    check(find_text(root, "Loading") != nullptr, "the fallback subtree is attached");
    check(find_text(root, "Query: ") == nullptr, "the content subtree is detached");

    // --- subtree restored on data -----------------------------------------------------------
    check(feed.pending_resolve(epoch, 7), "the completion for the current epoch lands");
    feed.pending_sync();
    check(!feed.pending_fallback_live(), "the boundary restores its content on data");
    check(find_text(root, "Loading") == nullptr, "the fallback is detached again");
    check(find_text(root, "Query: ") != nullptr, "the content subtree is attached again");

    // --- old subtree kept on Refreshing -----------------------------------------------------
    const std::uint64_t refresh = feed.pending_begin_fetch();
    check(refresh == epoch + 1, "a key change bumps the epoch");
    feed.pending_sync();
    check(!feed.pending_fallback_live(), "Refreshing keeps the old subtree (ADR-067 item 3)");
    check(find_text(root, "Query: ") != nullptr, "the content stays attached while refreshing");
    check(feed.pending_resolve(refresh, 8), "the refresh lands");
    feed.pending_sync();
    check(!feed.pending_fallback_live(), "the content stays live after the refresh");

    // --- a stale epoch is dropped -----------------------------------------------------------
    const std::uint64_t first = feed.pending_begin_fetch();
    const std::uint64_t second = feed.pending_begin_fetch();
    check(!feed.pending_resolve(first, 99), "a completion for a superseded epoch is dropped");
    check(feed.pending_epoch() == second, "the resource keeps the newer epoch");
    feed.pending_sync();
    check(!feed.pending_fallback_live(), "a dropped completion does not change the phase");
    check(feed.pending_resolve(second, 9), "the current epoch still lands");

    // --- isPending during a transition ------------------------------------------------------
    check(!feed.pending_is_pending(), "no transition is pending before a marked setter runs");
    // The marked setter is the Button's click binding: `startTransition(() => setQuery("next"))`.
    ui::Node* button = const_cast<ui::Node*>(find_kind(root, "Button"));
    check(button != nullptr, "the generated Button is attached");
    if (button != nullptr) {
        // The key change the setter causes starts the fetch, so the fetch is in flight before the
        // marked handler runs: that is the order `useResource` gives (ADR-067 item 1, "no fetch
        // happens in render: the hook only subscribes").
        const std::uint64_t transition_epoch = feed.pending_begin_fetch();
        ui::Event click;
        click.type = ui::EventType::Click;
        check(button->run_event_bindings(click) == 1, "the Button has the generated click binding");
        check(feed.pending_is_pending(), "a marked setter makes isPending true");
        check(feed.query() == "next", "the marked setter still assigns its value");
        check(feed.pending_epoch() == transition_epoch,
              "the mark started under the change's epoch");
        // The transition settles when its fetch completes.
        check(feed.pending_resolve(transition_epoch, 10), "the transition's fetch lands");
        feed.pending_sync();
        check(feed.pending_is_pending(), "isPending stays true until the mark observes the settle");
        check(feed.pending_observe_transition(), "the mark accepts its own epoch");
        check(!feed.pending_is_pending(), "the settled observation clears isPending");
    }

    // --- the update stays allocation-free ---------------------------------------------------
    // Warm up first: the first swap may grow a vector, and the gate is about the steady state a
    // frame runs in, not about the first build.
    for (int i = 0; i < 3; ++i) {
        const std::uint64_t warm = feed.pending_begin_fetch();
        feed.pending_sync();
        (void)feed.pending_resolve(warm, i);
        feed.pending_sync();
    }
    const std::size_t before = g_allocations;
    for (int i = 0; i < 64; ++i) {
        const std::uint64_t cycle = feed.pending_begin_fetch();
        feed.pending_sync(); // fallback (no data) or content (refreshing)
        (void)feed.pending_resolve(cycle, i);
        feed.pending_sync(); // back to content
        (void)feed.pending_is_pending();
        (void)feed.pending_fallback_live();
    }
    check(g_allocations == before, "64 transition cycles perform no allocation after warm-up");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pending runtime tests passed\n";
    return 0;
}
