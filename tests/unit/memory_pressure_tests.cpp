// SPDX-License-Identifier: MIT
//
// The ordered memory-pressure entry point (issue #705, memory-model-v1 section 7).
//
// What this asserts, and why each one matters:
//
// * The eviction order is the documented one, largest-first strictly. The order is the owner's
//   decision recorded on the issue, so a test that restated it in a different sequence would be a
//   silent behaviour change; asserting it against `eviction_order()` is what keeps the decision and
//   the code in step.
// * Registration order does not affect eviction order. A caller that registers the measure cache
//   first must still see the image cache evicted first, or the order is a property of the caller.
// * `should_retry` is true only when something was released. A retry that cannot succeed burns the
//   one attempt section 7 allows, so offering it on a no-op eviction is a bug.
// * A cache that cannot evict is still counted as asked. "Asked and had nothing" is different from
//   "not asked", and the diagnostic needs to tell them apart.

#include "tessera/rendering/memory/memory_pressure.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using tessera::rendering::memory::EvictableCache;
using tessera::rendering::memory::eviction_rank;
using tessera::rendering::memory::MemoryPressure;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "memory_pressure: " << message << "\n";
    }
    return condition;
}

// A cache that records the order it was asked in, so the test can assert the sequence rather than
// only the total.
struct FakeCache {
    std::string name;
    std::uint64_t bytes = 0;
    std::uint64_t release = 0;
};

EvictableCache make_cache(FakeCache& fake, std::vector<std::string>* log) {
    EvictableCache cache;
    cache.name = fake.name;
    cache.live_bytes = [&fake] { return fake.bytes; };
    cache.evict_idle = [&fake, log] {
        if (log != nullptr) {
            log->push_back(fake.name);
        }
        const std::uint64_t released = fake.release;
        fake.bytes -= released;
        fake.release = 0;
        return released;
    };
    return cache;
}

bool check_order() {
    const auto order = MemoryPressure::eviction_order();
    bool ok = expect(order.size() == 5, "the documented order has five caches");
    // Largest-first strictly, which is the owner's decision on the issue.
    ok = expect(order[0] == "image_texture", "image texture is evicted first") && ok;
    ok = expect(order[1] == "path_texture", "path texture is evicted second") && ok;
    ok = expect(order[2] == "paint", "paint is evicted third") && ok;
    ok = expect(order[3] == "glyph_atlas", "the glyph atlas is evicted fourth") && ok;
    ok = expect(order[4] == "text_measure", "the measure cache is evicted last") && ok;
    // The rank function agrees with the array, and an unknown name sorts after every known one.
    ok = expect(eviction_rank("image_texture") == 0, "rank of the first cache is 0") && ok;
    ok = expect(eviction_rank("text_measure") == 4, "rank of the last cache is 4") && ok;
    ok = expect(eviction_rank("not_a_cache") == order.size(),
                "an unordered cache ranks after every ordered one") &&
         ok;
    return ok;
}

bool check_registration_order_does_not_matter() {
    // Registered in the reverse of the documented order, evicted in the documented order.
    FakeCache measure{"text_measure", 100, 100};
    FakeCache glyph{"glyph_atlas", 200, 200};
    FakeCache paint{"paint", 300, 300};
    FakeCache path{"path_texture", 400, 400};
    FakeCache image{"image_texture", 500, 500};

    std::vector<std::string> log;
    MemoryPressure pressure;
    pressure.add(make_cache(measure, &log));
    pressure.add(make_cache(glyph, &log));
    pressure.add(make_cache(paint, &log));
    pressure.add(make_cache(path, &log));
    pressure.add(make_cache(image, &log));

    const auto result = pressure.relieve();
    bool ok = expect(log.size() == 5, "every registered cache was asked");
    ok = expect(log.size() == 5 && log[0] == "image_texture", "the image cache went first") && ok;
    ok = expect(log.size() == 5 && log[1] == "path_texture", "the path cache went second") && ok;
    ok = expect(log.size() == 5 && log[2] == "paint", "paint went third") && ok;
    ok = expect(log.size() == 5 && log[3] == "glyph_atlas", "the glyph atlas went fourth") && ok;
    ok = expect(log.size() == 5 && log[4] == "text_measure", "the measure cache went last") && ok;
    ok =
        expect(result.released_bytes == 1500, "the released bytes are the sum of the caches") && ok;
    ok = expect(result.caches_evicted == 5, "all five caches were counted as asked") && ok;
    ok = expect(result.should_retry, "a release that freed bytes offers a retry") && ok;
    return ok;
}

bool check_no_release_offers_no_retry() {
    FakeCache empty{"image_texture", 0, 0};
    MemoryPressure pressure;
    pressure.add(make_cache(empty, nullptr));
    const auto result = pressure.relieve();
    bool ok = expect(result.released_bytes == 0, "an empty cache releases nothing");
    ok = expect(result.caches_evicted == 1, "an empty cache is still counted as asked") && ok;
    // The important one: a retry that cannot succeed must not be offered.
    ok = expect(!result.should_retry, "a no-op eviction does not offer a retry") && ok;
    return ok;
}

bool check_unordered_cache_is_evicted_last() {
    FakeCache unknown{"some_new_cache", 50, 50};
    FakeCache image{"image_texture", 500, 500};
    std::vector<std::string> log;
    MemoryPressure pressure;
    pressure.add(make_cache(unknown, &log));
    pressure.add(make_cache(image, &log));
    const auto result = pressure.relieve();
    bool ok = expect(log.size() == 2 && log[0] == "image_texture",
                     "an ordered cache goes before an unordered one");
    ok = expect(log.size() == 2 && log[1] == "some_new_cache",
                "an unordered cache is evicted last rather than skipped") &&
         ok;
    ok = expect(result.released_bytes == 550, "both caches contributed") && ok;
    return ok;
}

bool check_live_bytes() {
    FakeCache image{"image_texture", 500, 0};
    FakeCache paint{"paint", 300, 0};
    MemoryPressure pressure;
    pressure.add(make_cache(image, nullptr));
    pressure.add(make_cache(paint, nullptr));
    bool ok = expect(pressure.total_live_bytes() == 800, "live bytes sum over the caches");
    ok = expect(pressure.cache_count() == 2, "the cache count is the registered count") && ok;
    return ok;
}

} // namespace

int main() {
    if (!check_order()) {
        return 1;
    }
    if (!check_registration_order_does_not_matter()) {
        return 2;
    }
    if (!check_no_release_offers_no_retry()) {
        return 3;
    }
    if (!check_unordered_cache_is_evicted_last()) {
        return 4;
    }
    if (!check_live_bytes()) {
        return 5;
    }
    std::cout << "memory_pressure: ok\n";
    return 0;
}
