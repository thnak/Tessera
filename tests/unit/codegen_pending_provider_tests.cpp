// SPDX-License-Identifier: MIT

// #656 follow-ups (docs/specs/pending-ui-v1.md section 6) on the compiled generated component
// `pending::ProviderFeed`:
//
// - the `ResourceProvider` service binds a view per resource key with no host-side bind call, and a
//   key change rebinds and starts the next fetch;
// - typed `.data` (string and int) and the `.error` text reach the bindings;
// - bindings that read `res::`, `isPending` or a deferred value refresh through the update
// scheduler
//   after `pending_sync()`, with no `update()` call;
// - an effect over `row.status` re-runs when the status changes;
// - a resource the host bound by hand is left alone;
// - steady-state sync, flush and key sync allocate nothing.

#include "ProviderFeed.hpp"
#include "demo/feed_api.hpp"
#include "demo/services.hpp"
#include "tessera/appfw/service_context.hpp"
#include "tessera/detail/aligned_alloc.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/data_store.hpp"
#include "tessera/ui/pending/fake_resource.hpp"
#include "tessera/ui/pending/resource_provider.hpp"
#include "tessera/ui/pending/store_resource.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
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

static_assert(pend::ResourceProvider<demo::FeedApi>, "the fixture service is a provider");
static_assert(!pend::ResourceProvider<demo::Navigator>, "a plain service is not a provider");

// A provider over the real data store: one StoreResource per (method, key), a fetch started the
// first time a key is seen. The method decides the scalar type of the published value.
class StoreFeedApi final : public demo::FeedApi {
public:
    explicit StoreFeedApi(runtime::DataStore& store)
        : store_(store) {}

    pend::ResourceView resource(std::string_view method, std::string_view key) noexcept override {
        for (const std::unique_ptr<Slot>& slot : slots_) {
            if (slot->method == method && slot->key == key) {
                return pend::ResourceView::of(*slot->resource);
            }
        }
        ++requests_;
        try {
            runtime::DataKey data_key{std::string(method), "app", {}, {}, std::string(key)};
            auto slot = std::make_unique<Slot>();
            slot->method = std::string(method);
            slot->key = std::string(key);
            slot->data_key = data_key;
            if (method == "loadRow") {
                slot->resource.reset(new pend::StoreResource(
                    pend::StoreResource::typed<std::string>(store_, data_key)));
            } else {
                slot->resource.reset(
                    new pend::StoreResource(pend::StoreResource::typed<int>(store_, data_key)));
            }
            slot->fetch = store_.begin_fetch(data_key);
            slots_.push_back(std::move(slot));
            return pend::ResourceView::of(*slots_.back()->resource);
        } catch (...) {
            return {};
        }
    }

    struct Slot {
        std::string method;
        std::string key;
        runtime::DataKey data_key;
        std::unique_ptr<pend::StoreResource> resource;
        runtime::FetchHandle fetch;
    };

    Slot* find(std::string_view method, std::string_view key) {
        for (const std::unique_ptr<Slot>& slot : slots_) {
            if (slot->method == method && slot->key == key) {
                return slot.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] int requests() const {
        return requests_;
    }

private:
    runtime::DataStore& store_;
    std::vector<std::unique_ptr<Slot>> slots_;
    int requests_ = 0;
};

std::string_view text_of(const ui::Node& node) {
    const std::optional<ui::PaintData>& paint = node.paint_data();
    return paint ? std::string_view(paint->text) : std::string_view();
}

std::string text_starting(const ui::Node& node, std::string_view prefix) {
    if (text_of(node).rfind(prefix, 0) == 0) {
        return std::string(text_of(node));
    }
    for (const auto& child : node.children()) {
        if (std::string found = text_starting(*child, prefix); !found.empty()) {
            return found;
        }
    }
    return {};
}

void test_provider_binding() {
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(16);
    runtime::DataStore store(queue, clock, 16);
    StoreFeedApi api(store);
    tessera::appfw::ServiceContext services;
    check(services.provide<demo::FeedApi>(api).has_value(), "the provider service is provided");

    pending::ProviderFeed feed;
    ui::UpdateScheduler scheduler;
    ui::Node root(4000);
    feed.build(root);
    feed.attach_scheduler(scheduler);
    check(api.requests() == 0, "nothing is requested before the service is known");
    feed.mount(services);

    // The service was asked once per resource, with the canonical key, and no bind call was made.
    check(api.requests() == 2, "mount binds both resources through the provider");
    check(api.find("loadRow", "id=i1;") != nullptr, "the record key is canonical: id=i1;");
    check(api.find("loadCount", "=i1;") != nullptr, "a bare scalar key is the empty-named field");
    check_eq(feed.res_row_status(), "loading", "the row is loading after mount");
    check_eq(text_starting(root, "Row:"), "Row:  / loading / [] / 0",
             "an absent typed .data reads as the zero value");
    check(feed.effect_runs() == 1, "the effect over row.status ran at mount");

    (void)scheduler.flush(); // the mount-time schedule

    // Completion: publish, then sync. The scheduler (not an update() call) refreshes the binding.
    StoreFeedApi::Slot* row = api.find("loadRow", "id=i1;");
    StoreFeedApi::Slot* count = api.find("loadCount", "=i1;");
    check(row->fetch.resolve(std::make_shared<const std::string>("hello")), "the row lands");
    check(count->fetch.resolve(std::make_shared<const int>(42)), "the count lands");
    check(!scheduler.pending(), "nothing is scheduled before the host syncs");
    feed.pending_sync();
    check(scheduler.pending(), "a sync that saw a change schedules the update");
    (void)scheduler.flush();
    check_eq(text_starting(root, "Row:"), "Row: hello / success / [] / 42",
             "typed string and int .data reach the binding without update()");
    check(feed.effect_runs() == 2, "the effect re-ran when row.status changed");

    // A second sync with nothing new schedules nothing.
    feed.pending_sync();
    check(!scheduler.pending(), "an unchanged sync does not schedule");

    // A failed refresh keeps the data and shows the error text.
    runtime::FetchHandle again = store.begin_fetch(row->data_key);
    feed.pending_sync();
    (void)scheduler.flush();
    check_eq(feed.res_row_status(), "refreshing", "a refresh keeps the data");
    check(again.fail(runtime::DataError{"E_NET", "boom"}), "the failure lands");
    feed.pending_sync();
    (void)scheduler.flush();
    check_eq(text_starting(root, "Row:"), "Row: hello / error / [boom] / 42",
             ".error is the failure text and the old data stays");
    check(feed.effect_runs() == 4, "the effect saw refreshing and then error");

    // A marked key change rebinds through the provider and holds isPending until the new key lands.
    check_eq(text_starting(root, "Pending:"), "Pending: false / Level: 0",
             "idle before the change");
    feed.run_marked_set();
    check(api.requests() == 4, "the key change asked the provider for the new keys");
    check(feed.pending_is_pending(), "isPending is true from the marked change");
    check_eq(text_starting(root, "Pending:"), "Pending: true / Level: 0",
             "the binding that reads isPending refreshed in the handler's own update");
    StoreFeedApi::Slot* next_row = api.find("loadRow", "id=i2;");
    StoreFeedApi::Slot* next_count = api.find("loadCount", "=i2;");
    check(next_row != nullptr && next_count != nullptr, "the new keys have their own entries");
    check(next_row->fetch.resolve(std::make_shared<const std::string>("second")),
          "the new row lands");
    check(next_count->fetch.resolve(std::make_shared<const int>(7)), "the new count lands");
    feed.pending_sync();
    (void)scheduler.flush();
    check(!feed.pending_is_pending(), "isPending clears when the new key's fetch settles");
    check_eq(text_starting(root, "Pending:"), "Pending: false / Level: 0",
             "isPending refreshed without update() after the sync");
    check_eq(text_starting(root, "Row:"), "Row: second / success / [] / 7",
             "the bindings follow the new key");

    // The provider is not asked again while the key is unchanged.
    const int requests = api.requests();
    for (int i = 0; i < 8; ++i) {
        feed.pending_sync();
    }
    check(api.requests() == requests, "an unchanged key does not rebind");

    feed.detach_scheduler(scheduler);
}

void test_manual_bind_wins() {
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(16);
    runtime::DataStore store(queue, clock, 16);
    StoreFeedApi api(store);
    tessera::appfw::ServiceContext services;
    check(services.provide<demo::FeedApi>(api).has_value(), "the provider service is provided");

    pending::ProviderFeed feed;
    ui::Node root(4100);
    feed.build(root);
    pend::FakeResource fake;
    feed.bind_res_row(pend::ResourceView::of(fake));
    feed.mount(services);
    check(api.find("loadRow", "id=i1;") == nullptr,
          "a hand-bound resource is not asked of the provider");
    check(api.find("loadCount", "=i1;") != nullptr, "the other resource still is");
    const std::uint64_t epoch = fake.begin_fetch();
    check(fake.fail(epoch, "offline"), "the fake fails with a message");
    feed.pending_sync();
    feed.update();
    check_eq(text_starting(root, "Row:"), "Row:  / error / [offline] / 0",
             "a fake resource's failure text reaches .error");
}

void test_allocation_free() {
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(16);
    runtime::DataStore store(queue, clock, 16);
    StoreFeedApi api(store);
    tessera::appfw::ServiceContext services;
    check(services.provide<demo::FeedApi>(api).has_value(), "the provider service is provided");

    pending::ProviderFeed feed;
    ui::UpdateScheduler scheduler;
    scheduler.reserve(4);
    ui::Node root(4200);
    feed.build(root);
    feed.attach_scheduler(scheduler);
    feed.mount(services);
    StoreFeedApi::Slot* row = api.find("loadRow", "id=i1;");
    StoreFeedApi::Slot* count = api.find("loadCount", "=i1;");
    (void)row->fetch.resolve(std::make_shared<const std::string>("hello"));
    (void)count->fetch.resolve(std::make_shared<const int>(42));
    for (int warm = 0; warm < 3; ++warm) {
        feed.pending_sync();
        (void)scheduler.flush();
    }
    const std::size_t before = g_allocations;
    for (int i = 0; i < 256; ++i) {
        feed.pending_sync();
        (void)scheduler.flush();
        feed.update();
    }
    check(g_allocations == before, "steady-state sync, flush and update allocate nothing");
    feed.detach_scheduler(scheduler);
}

} // namespace

int main() {
    test_provider_binding();
    test_manual_bind_wins();
    test_allocation_free();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pending provider runtime: ok\n";
    return 0;
}
