// SPDX-License-Identifier: MIT

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/data_store.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using tessera::runtime::CompletionQueue;
using tessera::runtime::DataError;
using tessera::runtime::DataKey;
using tessera::runtime::DataSnapshot;
using tessera::runtime::DataStatus;
using tessera::runtime::DataStore;
using tessera::runtime::FetchHandle;
using tessera::runtime::ManualClock;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "data_store: " << message << "\n";
    }
    return condition;
}

DataKey key(std::string type, std::string scope = "app") {
    return DataKey{std::move(type), std::move(scope), {}, {}, {}};
}

template<typename T> std::shared_ptr<const T> value(T item) {
    return std::make_shared<const T>(std::move(item));
}

} // namespace

int main() {
    // set: writes the entry, marks it fresh, and notifies once on the draining thread.
    {
        ManualClock clock(1.0);
        CompletionQueue queue(16);
        DataStore store(queue, clock, 8);
        const DataKey k = key("users");
        std::vector<DataSnapshot> observed;
        store.observe(k,
                      [&observed](const DataSnapshot& snapshot) { observed.push_back(snapshot); });

        store.set(k, value(42));
        const DataSnapshot written = store.snapshot(k);
        const std::shared_ptr<const int> read = store.get<int>(k);
        if (!expect(observed.empty(), "set does not notify inline") ||
            !expect(queue.size() == 1, "set posts one tick notification") ||
            !expect(written.status == DataStatus::Success, "set marks the entry fresh") ||
            !expect(written.version == 1, "first write has version 1") ||
            !expect(written.updated_at == 1.0, "set stamps updated_at from the clock") ||
            !expect(read != nullptr && *read == 42, "set stores the pushed value") ||
            !expect(store.get<std::string>(k) == nullptr, "typed read rejects another type")) {
            return 1;
        }
        queue.run(1);
        const std::shared_ptr<const int> seen =
            observed.size() == 1 ? observed[0].value_as<int>() : nullptr;
        if (!expect(observed.size() == 1, "exactly one notification at the tick boundary") ||
            !expect(seen != nullptr && *seen == 42, "observer sees the pushed value") ||
            !expect(store.notifications_delivered() == 1, "delivery is counted once")) {
            return 2;
        }
    }

    // update: last write wins, coalesced, and it can seed an absent key.
    {
        ManualClock clock;
        CompletionQueue queue(16);
        DataStore store(queue, clock, 8);
        const DataKey k = key("counter");
        std::vector<DataSnapshot> observed;
        store.observe(k,
                      [&observed](const DataSnapshot& snapshot) { observed.push_back(snapshot); });

        store.set(k, value(10));
        if (!store.update<int>(k,
                               [](const int* previous) { return previous ? *previous + 5 : 1; })) {
            return 10;
        }
        if (!expect(observed.empty(), "pushes never notify inline") ||
            !expect(queue.size() == 1, "pushes coalesce into one notification task")) {
            return 11;
        }
        queue.run(4);
        const std::shared_ptr<const int> seen =
            observed.size() == 1 ? observed[0].value_as<int>() : nullptr;
        if (!expect(observed.size() == 1, "coalesced notification runs once") ||
            !expect(observed[0].version == 2, "update bumps the monotonic version") ||
            !expect(seen != nullptr && *seen == 15, "update transforms the current value")) {
            return 12;
        }

        const DataKey absent = key("empty");
        const bool seeded =
            store.update<int>(absent, [](const int* previous) { return previous ? *previous : 7; });
        const std::shared_ptr<const int> seeded_value = store.get<int>(absent);
        if (!expect(seeded, "update initializes an absent key") ||
            !expect(seeded_value != nullptr && *seeded_value == 7,
                    "update writes the initial value") ||
            !expect(store.snapshot(absent).version == 1, "initial update is version 1") ||
            !expect(!store.update<std::string>(absent,
                                               [](const std::string*) { return std::string("x"); }),
                    "update refuses a different type")) {
            return 13;
        }
    }

    // Versions are monotonic per key and last write wins.
    {
        ManualClock clock;
        CompletionQueue queue(16);
        DataStore store(queue, clock, 8);
        const DataKey k = key("last-write");
        std::vector<DataSnapshot> observed;
        store.observe(k,
                      [&observed](const DataSnapshot& snapshot) { observed.push_back(snapshot); });

        store.set(k, value(1));
        queue.run(2);
        store.set(k, value(2));
        queue.run(2);
        store.set(k, value(3));

        const DataSnapshot final_snapshot = store.snapshot(k);
        const std::shared_ptr<const int> final_value = final_snapshot.value_as<int>();
        if (!expect(observed.size() == 2, "one notification per tick") ||
            !expect(observed[0].version == 1 && observed[1].version == 2, "versions increase") ||
            !expect(final_snapshot.version == 3, "latest write has the highest version") ||
            !expect(final_value != nullptr && *final_value == 3, "last write wins")) {
            return 20;
        }
    }

    // A push supersedes an in-flight fetch: token cancelled, late result discarded.
    {
        ManualClock clock(5.0);
        CompletionQueue queue(16);
        DataStore store(queue, clock, 8);
        const DataKey k = key("profile");
        std::vector<DataSnapshot> observed;
        store.observe(k,
                      [&observed](const DataSnapshot& snapshot) { observed.push_back(snapshot); });

        FetchHandle fetch = store.begin_fetch(k);
        bool cancelled = false;
        fetch.token().on_cancel([&cancelled] { cancelled = true; });
        if (!expect(fetch.valid(), "begin_fetch returns a handle") ||
            !expect(store.snapshot(k).status == DataStatus::Loading, "fetch starts Loading") ||
            !expect(queue.size() == 1, "fetch start posts a tick notification")) {
            return 30;
        }
        queue.run(4);
        if (!expect(observed.size() == 1 && observed[0].status == DataStatus::Loading,
                    "Loading is observable at the tick")) {
            return 31;
        }

        store.set(k, value(std::string("pushed")));
        queue.run(4);
        if (!expect(cancelled, "push cancels the in-flight fetch token") ||
            !expect(observed.size() == 2, "push notifies once") ||
            !expect(observed[1].status == DataStatus::Success && observed[1].version == 1,
                    "push publishes Success at version 1") ||
            !expect(observed[1].value_as<std::string>() != nullptr &&
                        *observed[1].value_as<std::string>() == "pushed",
                    "observer sees the pushed value")) {
            return 32;
        }

        if (!expect(!fetch.resolve(value(std::string("late"))),
                    "late fetch success is discarded") ||
            !expect(!fetch.fail(DataError{"timeout", "late"}), "late fetch error is discarded")) {
            return 33;
        }
        queue.run(4);
        const DataSnapshot final_snapshot = store.snapshot(k);
        const std::shared_ptr<const std::string> final_value =
            final_snapshot.value_as<std::string>();
        if (!expect(final_snapshot.version == 1, "late fetch does not bump the version") ||
            !expect(final_value != nullptr && *final_value == "pushed",
                    "late fetch cannot overwrite a push") ||
            !expect(observed.size() == 2, "discarded fetch does not notify")) {
            return 34;
        }
    }

    // Fetch completion obeys the same version ordering as pushes.
    {
        ManualClock clock;
        CompletionQueue queue(16);
        DataStore store(queue, clock, 8);
        const DataKey k = key("fetch");
        FetchHandle first = store.begin_fetch(k);
        FetchHandle duplicate = store.begin_fetch(k);
        if (!expect(first.resolve(value(5)), "fetch result publishes") ||
            !expect(!duplicate.resolve(value(6)), "deduped duplicate result is discarded")) {
            return 40;
        }
        queue.run(4);
        const DataSnapshot resolved = store.snapshot(k);
        if (!expect(resolved.status == DataStatus::Success && resolved.version == 1,
                    "fetch success is version 1") ||
            !expect(resolved.value_as<int>() != nullptr && *resolved.value_as<int>() == 5,
                    "fetch result stored")) {
            return 41;
        }

        store.set(k, value(7));
        queue.run(4);
        const DataSnapshot pushed = store.snapshot(k);
        if (!expect(pushed.version == 2, "push after a fetch bumps the version") ||
            !expect(pushed.value_as<int>() != nullptr && *pushed.value_as<int>() == 7,
                    "push after a fetch wins")) {
            return 42;
        }

        FetchHandle refresh = store.begin_fetch(k);
        if (!expect(store.snapshot(k).status == DataStatus::Refreshing,
                    "refresh keeps previous data") ||
            !expect(refresh.fail(DataError{"offline", "no network"}),
                    "refresh failure is published") ||
            !expect(queue.run(4) == 1, "refresh failure notifies at the tick")) {
            return 43;
        }
        const DataSnapshot failed = store.snapshot(k);
        if (!expect(failed.status == DataStatus::Error && failed.version == 2,
                    "failure keeps the data version") ||
            !expect(failed.value_as<int>() != nullptr && *failed.value_as<int>() == 7,
                    "failure retains the previous published data") ||
            !expect(failed.error.has_value() && failed.error->code == "offline",
                    "failure carries the value error")) {
            return 44;
        }
    }

    // Pushed entries share the bounded cache and its deterministic LRU eviction.
    {
        ManualClock clock;
        CompletionQueue queue(16);
        DataStore store(queue, clock, 2);
        const DataKey a = key("a");
        const DataKey b = key("b");
        const DataKey c = key("c");
        const DataKey d = key("d");
        store.set(a, value(1));
        store.set(b, value(2));
        store.set(c, value(3));
        if (!expect(store.size() == 2, "pushed entries obey the cache bound") ||
            !expect(store.capacity() == 2, "capacity is reported") ||
            !expect(store.snapshot(a).status == DataStatus::Idle, "oldest push was evicted") ||
            !expect(store.get<int>(b) != nullptr && *store.get<int>(b) == 2,
                    "newer push retained")) {
            return 50;
        }
        (void)store.snapshot(b);
        store.set(d, value(4));
        if (!expect(store.size() == 2, "still bounded after more pushes") ||
            !expect(store.snapshot(c).status == DataStatus::Idle,
                    "least recently used entry evicted") ||
            !expect(store.get<int>(d) != nullptr && *store.get<int>(d) == 4,
                    "newest push retained")) {
            return 51;
        }
    }

    // A worker thread push never runs an observer; the UI thread's tick does.
    {
        ManualClock clock;
        CompletionQueue queue(16);
        DataStore store(queue, clock, 8);
        const DataKey k = key("threaded");
        const std::thread::id ui_thread = std::this_thread::get_id();
        bool notified = false;
        std::thread::id callback_thread;
        store.observe(k, [&notified, &callback_thread](const DataSnapshot&) {
            notified = true;
            callback_thread = std::this_thread::get_id();
        });

        std::thread worker([&store, &k] { store.set(k, value(1)); });
        worker.join();
        if (!expect(!notified, "worker push does not run observers on the worker thread") ||
            !expect(queue.size() == 1, "worker push posts one tick notification") ||
            !expect(store.get<int>(k) != nullptr, "worker push wrote the cache")) {
            return 60;
        }
        queue.run(1);
        if (!expect(notified, "observer runs when the UI drains the tick") ||
            !expect(callback_thread == ui_thread, "observer runs on the UI thread")) {
            return 61;
        }
    }

    // unobserve is per-id, and a destroyed store never calls its observers.
    {
        ManualClock clock;
        CompletionQueue queue(16);
        const DataKey k = key("observers");
        int first = 0;
        int second = 0;
        {
            DataStore store(queue, clock, 8);
            const DataStore::ObserverId first_id =
                store.observe(k, [&first](const DataSnapshot&) { ++first; });
            store.observe(k, [&second](const DataSnapshot&) { ++second; });
            store.unobserve(first_id);
            store.set(k, value(1));
            queue.run(1);
            if (!expect(first == 0 && second == 1, "unobserve removes exactly one observer")) {
                return 70;
            }
            store.set(k, value(2));
        }
        queue.run(1);
        if (!expect(second == 1, "no observer callbacks after store destruction")) {
            return 71;
        }
    }

    std::cout << "data_store: ok\n";
    return 0;
}
