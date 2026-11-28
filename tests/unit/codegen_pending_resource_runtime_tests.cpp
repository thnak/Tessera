// SPDX-License-Identifier: MIT

// #656 (ADR-067 item 1), runtime side: the resource-driven `ResourceFeed` component compiled and
// driven through ResourceViews. The boundary really swaps scene subtrees as the resources load, the
// subtree survives a refresh, a stale epoch is dropped, a failed refresh keeps the data, and the
// update path allocates nothing after warm-up. The same component then reads the real data store
// through `StoreResource`.

#include "ResourceFeed.hpp"
#include "tessera/detail/aligned_alloc.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/data_store.hpp"
#include "tessera/ui/pending/fake_resource.hpp"
#include "tessera/ui/pending/resource_view.hpp"
#include "tessera/ui/pending/store_resource.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace ui = tessera::ui;
namespace pend = tessera::ui::pending;
namespace runtime = tessera::runtime;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::size_t g_allocations = 0;

} // namespace

void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* block = std::malloc(size)) {
        return block;
    }
    throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    ++g_allocations;
    return tessera::detail::aligned_allocate(size, alignment);
}

void operator delete(void* pointer, std::align_val_t) noexcept {
    tessera::detail::aligned_release(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    tessera::detail::aligned_release(pointer);
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

namespace {

std::string_view text_of(const ui::Node& node) {
    const std::optional<ui::PaintData>& paint = node.paint_data();
    return paint ? std::string_view(paint->text) : std::string_view();
}

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

// The node of the boundary's content subtree (its status text), or null while it is detached.
const ui::Node* content_text(const ui::Node& root) {
    const std::function<const ui::Node*(const ui::Node&)> search =
        [&search](const ui::Node& node) -> const ui::Node* {
        if (text_of(node).rfind("Query:", 0) == 0) {
            return &node;
        }
        for (const auto& child : node.children()) {
            if (const ui::Node* found = search(*child)) {
                return found;
            }
        }
        return nullptr;
    };
    return search(root);
}

void test_fake_resources() {
    pending::ResourceFeed feed;
    ui::Node root(1000);
    feed.build(root);
    pend::FakeResource data;
    pend::FakeResource tags;

    check(!feed.pending_fallback_live(), "unbound resources read as idle: the content is live");
    check(content_text(root) != nullptr, "the content subtree is attached at build");
    check(std::string_view(feed.res_feed_status()) == "idle", "an unbound view is idle");

    feed.bind_res_feed(pend::ResourceView::of(data));
    feed.bind_res_tags(pend::ResourceView::of(tags));
    check(!feed.pending_fallback_live(), "bound idle resources do not suspend");

    // Fallback while a suspending read is Loading with no data.
    const std::uint64_t first = data.begin_fetch();
    feed.pending_sync();
    check(feed.pending_fallback_live(), "a suspending read with no data shows the fallback");
    check(find_text(root, "Loading") != nullptr && content_text(root) == nullptr,
          "the fallback subtree replaced the content in the scene");
    check(std::string_view(feed.res_feed_status()) == "loading", "status is loading");

    // Restore, with the content subtree kept (not rebuilt).
    check(data.resolve(first, 7), "the completion for the current epoch lands");
    feed.pending_sync();
    check(!feed.pending_fallback_live(), "the data restores the content");
    const ui::Node* kept = content_text(root);
    check(kept != nullptr && find_text(root, "Loading") == nullptr, "the content is back");
    check(std::string_view(feed.res_feed_status()) == "success", "status is success");

    // Two read slots: the second resource suspends the same boundary.
    const std::uint64_t tag_epoch = tags.begin_fetch();
    feed.pending_sync();
    check(feed.pending_fallback_live(), "either covered read suspends the boundary");
    check(tags.resolve(tag_epoch, 1), "the tags completion lands");
    feed.pending_sync();
    check(!feed.pending_fallback_live(), "both reads have data");
    check(content_text(root) == kept, "the content subtree is the same node after a restore");

    // A refresh keeps the subtree: Refreshing never suspends.
    (void)data.begin_fetch();
    feed.pending_sync();
    check(!feed.pending_fallback_live() && content_text(root) == kept,
          "a refresh with data keeps the subtree");
    check(std::string_view(feed.res_feed_status()) == "refreshing", "status is refreshing");

    // A completion for a superseded epoch is dropped.
    const std::uint64_t stale = data.begin_fetch();
    const std::uint64_t current = data.begin_fetch();
    check(!data.resolve(stale, 99), "the superseded completion is dropped");
    check(data.resolve(current, 5), "the current completion lands");
    feed.pending_sync();
    check(!feed.pending_fallback_live() && data.value() == 5, "only the current value is applied");

    // A transition waits for the epoch the key change produces and clears when the fetch settles.
    check(!feed.pending_is_pending(), "no transition yet");
    feed.run_marked_set();
    check(feed.query() == "next", "the marked setter assigns its value");
    check(feed.pending_is_pending(), "isPending is true from the marked change");
    const std::uint64_t fetch = data.begin_fetch(); // the host's fetch for the new key
    feed.pending_sync();
    check(feed.pending_is_pending() && !feed.pending_fallback_live(),
          "isPending holds while the fetch runs and the old subtree stays");
    check(data.resolve(fetch, 6), "the key's fetch lands");
    feed.pending_sync();
    check(!feed.pending_is_pending(), "isPending clears when the read settles");

    // #960: the cell already holds "next", so the same marked setter starts no fetch and must not
    // leave isPending waiting for one.
    feed.run_marked_set();
    check(!feed.pending_is_pending(), "a no-op marked setter leaves isPending false");

    // A failed refresh keeps the data.
    const std::uint64_t failing = data.begin_fetch();
    check(data.fail(failing), "the failure lands");
    feed.pending_sync();
    check(!feed.pending_fallback_live() && std::string_view(feed.res_feed_status()) == "error",
          "a failed refresh keeps the subtree and reports the error status");

    // Unbinding goes back to idle rather than crashing.
    feed.bind_res_feed(pend::ResourceView{});
    check(std::string_view(feed.res_feed_status()) == "idle", "an unbound view is idle again");
}

void test_allocation_free() {
    pending::ResourceFeed feed;
    ui::Node root(2000);
    feed.build(root);
    pend::FakeResource data;
    pend::FakeResource tags;
    feed.bind_res_feed(pend::ResourceView::of(data));
    feed.bind_res_tags(pend::ResourceView::of(tags));

    const auto cycle = [&] {
        const std::uint64_t first = data.begin_fetch();
        feed.pending_sync(); // fallback on the first cycle, a refresh afterwards
        (void)data.resolve(first, 1);
        feed.pending_sync();
        feed.run_marked_set();
        const std::uint64_t fetch = data.begin_fetch();
        feed.pending_sync();
        (void)data.resolve(fetch, 2);
        feed.pending_sync();
    };
    for (int warm = 0; warm < 3; ++warm) {
        cycle();
    }
    // A cold boundary swap (a resource with no data) after warm-up.
    pend::FakeResource cold;
    feed.bind_res_tags(pend::ResourceView::of(cold));
    // Warm the new binding too: its status words are longer than the idle ones the text first held.
    for (int warm = 0; warm < 3; ++warm) {
        const std::uint64_t epoch = cold.begin_fetch();
        feed.pending_sync();
        (void)cold.resolve(epoch, 1);
        feed.pending_sync();
        cycle();
    }
    const std::size_t before = g_allocations;
    for (int i = 0; i < 64; ++i) {
        const std::uint64_t epoch = cold.begin_fetch();
        feed.pending_sync();
        (void)cold.resolve(epoch, 1);
        feed.pending_sync();
        cycle();
    }
    check(g_allocations == before, "resource updates, swaps and transitions allocate nothing");
}

void test_data_store() {
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(16);
    runtime::DataStore store(queue, clock, 8);
    const runtime::DataKey key{"feed", "app", {}, {}, "q=next"};
    const pend::StoreResource resource(store, key);

    pending::ResourceFeed feed;
    ui::Node root(3000);
    feed.build(root);
    feed.bind_res_feed(pend::ResourceView::of(resource));

    check(!feed.pending_fallback_live(), "an unwritten key does not suspend");
    runtime::FetchHandle first = store.begin_fetch(key);
    feed.pending_sync();
    check(feed.pending_fallback_live() && std::string_view(feed.res_feed_status()) == "loading",
          "a store fetch with no data shows the fallback");
    check(first.resolve(std::make_shared<const int>(7)), "the store completion lands");
    feed.pending_sync();
    check(!feed.pending_fallback_live() && std::string_view(feed.res_feed_status()) == "success",
          "the published data restores the content");

    runtime::FetchHandle second = store.begin_fetch(key);
    feed.pending_sync();
    check(!feed.pending_fallback_live() && std::string_view(feed.res_feed_status()) == "refreshing",
          "a store refresh keeps the subtree");
    store.set(key, std::make_shared<const int>(9)); // supersedes the fetch
    check(!second.resolve(std::make_shared<const int>(1)), "the store drops the stale completion");
    feed.pending_sync();
    check(std::string_view(feed.res_feed_status()) == "success", "the pushed value is current");

    // Reading the store from the UI thread allocates nothing.
    const std::size_t before = g_allocations;
    for (int i = 0; i < 1000; ++i) {
        feed.pending_sync();
    }
    check(g_allocations == before, "syncing against the data store allocates nothing");
}

} // namespace

int main() {
    test_fake_resources();
    test_allocation_free();
    test_data_store();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pending resource runtime: ok\n";
    return 0;
}
