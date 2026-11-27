// SPDX-License-Identifier: MIT
//
// OffscreenTargetPool directly (docs/design/gpu/offscreen-layers.md section 5e): release and reuse
// inside a frame and across frames, the legacy never-released rotation rule, growth and retirement
// of an in-flight target, both caps, idle trimming, the counters, and that the steady state does
// not allocate.

#include "tessera/rendering/gpu/offscreen_target_pool.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>

namespace {

// Counts every allocation of the process so the steady-state test can assert there are none.
std::size_t g_allocations = 0;

} // namespace

void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::OffscreenTargetPool;
using tessera::rendering::gpu::OffscreenTargetPoolConfig;

constexpr rhi::TextureFormat kFormat = rhi::TextureFormat::R8G8B8A8Unorm;
constexpr rhi::Extent2D kSmall{16, 16};
constexpr rhi::Extent2D kLarge{32, 32};
constexpr std::uint64_t kSmallBytes = 16 * 16 * 4;
constexpr std::uint64_t kLargeBytes = 32 * 32 * 4;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "offscreen_pool: " << message << "\n";
    }
    return condition;
}

struct Fixture {
    std::unique_ptr<rhi::Device> device = tessera::rendering::null::create_null_device({});
    std::unique_ptr<OffscreenTargetPool> pool;

    explicit Fixture(OffscreenTargetPoolConfig config = {}) {
        pool = std::make_unique<OffscreenTargetPool>(*device, config);
    }
    void frame(std::uint64_t index, std::uint32_t in_flight = 1) {
        pool->begin_frame(index, in_flight, kFormat);
    }
};

bool release_reuses_within_a_frame() {
    Fixture f;
    f.frame(1);
    rhi::RenderTarget* first = f.pool->acquire(kSmall);
    rhi::RenderTarget* second = f.pool->acquire(kSmall);
    bool ok = expect(first != nullptr && second != nullptr && first != second,
                     "two held targets are distinct") &&
              expect(f.pool->stats().live == 2 && f.pool->stats().peak_live == 2, "two live");
    f.pool->release(first);
    rhi::RenderTarget* third = f.pool->acquire(kSmall);
    return expect(third == first, "a released target comes back in the same frame") && ok &&
           expect(f.pool->target_count() == 2 && f.pool->created() == 2,
                  "without creating a third") &&
           expect(f.pool->stats().live == 2 && f.pool->stats().peak_live == 2,
                  "peak stays at the two that were ever live together");
}

bool release_is_idempotent_and_checked() {
    Fixture f;
    f.frame(1);
    rhi::RenderTarget* held = f.pool->acquire(kSmall);
    f.pool->release(held);
    f.pool->release(held);    // already free
    f.pool->release(nullptr); // nothing
    auto stranger = f.device->create_render_target(rhi::RenderTargetDesc{kSmall});
    f.pool->release(stranger.get()); // not the pool's
    return expect(f.pool->stats().live == 0, "a second or foreign release changes nothing") &&
           expect(f.pool->target_count() == 1, "the pool is intact");
}

bool released_target_serves_the_next_frame_at_any_ring_depth() {
    // The window's renderer rings 8 frames; a layer used every frame must not need 8 targets.
    Fixture f;
    for (std::uint64_t frame = 1; frame <= 40; ++frame) {
        f.frame(frame, 8);
        rhi::RenderTarget* target = f.pool->acquire(kSmall);
        if (target == nullptr) {
            return expect(false, "ring depth 8: every frame gets its target");
        }
        f.pool->release(target);
    }
    return expect(f.pool->target_count() == 1 && f.pool->created() == 1 &&
                      f.pool->stats().exhausted_total == 0,
                  "one target served forty frames at ring depth 8");
}

bool unreleased_target_waits_for_its_frame() {
    // The legacy rule for a caller that never releases: held until frames_in_flight rotated.
    Fixture f(OffscreenTargetPoolConfig{3, 0, 0});
    f.frame(1, 2);
    rhi::RenderTarget* held = f.pool->acquire(kSmall);
    f.frame(2, 2);
    rhi::RenderTarget* other = f.pool->acquire(kSmall);
    bool ok = expect(other != held, "frame 2 cannot take frame 1's target (still in flight)");
    f.frame(3, 2);
    rhi::RenderTarget* again = f.pool->acquire(kSmall);
    return expect(again == held, "frame 3 can: frame 1 completed") && ok;
}

bool in_flight_target_is_retired_not_destroyed() {
    Fixture f(OffscreenTargetPoolConfig{2, 0, 0});
    f.frame(1, 2);
    rhi::RenderTarget* small = f.pool->acquire(kSmall);
    f.pool->release(small);
    f.frame(2, 2);
    rhi::RenderTarget* large = f.pool->acquire(kLarge);
    bool ok = expect(large != nullptr && large->extent().width == 32,
                     "a larger request regrows the free slot") &&
              expect(f.pool->target_count() == 1, "in the same slot") &&
              expect(f.pool->stats().bytes == kSmallBytes + kLargeBytes,
                     "the old image stays alive and charged while frame 1 may still read it");
    f.frame(3, 2); // frame 1 has rotated
    ok = expect(f.pool->stats().bytes == kLargeBytes, "and is destroyed once it has") && ok;

    // A slot whose last frame already completed is regrown in place, nothing retired.
    f.pool->release(large);
    f.frame(5, 2);
    rhi::RenderTarget* larger = f.pool->acquire({64, 64});
    return expect(larger != nullptr && f.pool->stats().bytes == 64 * 64 * 4,
                  "a completed slot is replaced outright") &&
           ok;
}

bool count_cap_refuses_and_counts() {
    Fixture f(OffscreenTargetPoolConfig{2, 0, 0});
    f.frame(1);
    rhi::RenderTarget* a = f.pool->acquire(kSmall);
    rhi::RenderTarget* b = f.pool->acquire(kSmall);
    rhi::RenderTarget* c = f.pool->acquire(kSmall);
    bool ok = expect(a != nullptr && b != nullptr && c == nullptr, "the third is refused") &&
              expect(f.pool->stats().exhausted == 1 && f.pool->stats().exhausted_total == 1,
                     "and counted");
    f.frame(2);
    ok = expect(f.pool->stats().exhausted == 0 && f.pool->stats().exhausted_total == 1,
                "per-frame count resets, total stays") &&
         ok;
    Fixture off(OffscreenTargetPoolConfig{0, 0, 0});
    off.frame(1);
    return expect(off.pool->acquire(kSmall) == nullptr && off.pool->stats().exhausted_total == 0,
                  "a disabled pool is not an exhausted one") &&
           ok;
}

bool byte_cap_refuses_creation_and_growth() {
    Fixture f(OffscreenTargetPoolConfig{4, kSmallBytes + kSmallBytes / 2, 0});
    f.frame(1);
    rhi::RenderTarget* a = f.pool->acquire(kSmall);
    rhi::RenderTarget* b = f.pool->acquire(kSmall);
    bool ok = expect(a != nullptr && b == nullptr && f.pool->stats().bytes == kSmallBytes,
                     "a second target would pass the byte cap") &&
              expect(f.pool->stats().exhausted == 1, "and is counted");
    f.pool->release(a);
    f.frame(5);
    ok =
        expect(f.pool->acquire(kLarge) == nullptr, "growth past the byte cap is refused too") && ok;
    return expect(f.pool->stats().bytes == kSmallBytes, "nothing was destroyed for it") && ok;
}

bool idle_targets_are_trimmed() {
    Fixture f(OffscreenTargetPoolConfig{4, 0, 4});
    f.frame(1, 2);
    f.pool->release(f.pool->acquire(kSmall));
    for (std::uint64_t frame = 2; frame <= 4; ++frame) {
        f.frame(frame, 2);
    }
    bool ok = expect(f.pool->target_count() == 1, "idle for three frames: kept");
    f.frame(5, 2);
    ok = expect(f.pool->target_count() == 0 && f.pool->stats().bytes == 0 &&
                    f.pool->stats().trimmed_total == 1,
                "idle for four: destroyed") &&
         ok;

    Fixture g(OffscreenTargetPoolConfig{4, 0, 1});
    g.frame(1, 3);
    g.pool->release(g.pool->acquire(kSmall));
    g.frame(2, 3);
    g.frame(3, 3);
    ok = expect(g.pool->target_count() == 1,
                "a trim window below frames_in_flight is raised to it") &&
         ok;
    g.frame(4, 3);
    return expect(g.pool->target_count() == 0, "and applies once the frame completed") && ok;
}

bool explicit_trim() {
    Fixture f;
    f.frame(1, 2);
    rhi::RenderTarget* kept = f.pool->acquire(kSmall);
    f.pool->release(f.pool->acquire(kSmall));
    f.pool->trim(false);
    bool ok = expect(f.pool->target_count() == 2, "nothing is destroyed while its frame may run");
    f.pool->trim(true);
    ok = expect(f.pool->target_count() == 0 && f.pool->stats().bytes == 0,
                "a drained driver frees all, held or not") &&
         ok;
    (void)kept;
    f.frame(2, 2);
    return expect(f.pool->acquire(kSmall) != nullptr && f.pool->created() == 3,
                  "and the pool works again") &&
           ok;
}

bool steady_state_does_not_allocate() {
    Fixture f(OffscreenTargetPoolConfig{4, 0, 120});
    // Warm up: grow to the two targets and the largest size.
    f.frame(1, 2);
    rhi::RenderTarget* a = f.pool->acquire(kLarge);
    rhi::RenderTarget* b = f.pool->acquire(kLarge);
    f.pool->release(a);
    f.pool->release(b);
    const std::size_t before = g_allocations;
    for (std::uint64_t frame = 2; frame < 400; ++frame) {
        f.frame(frame, 2);
        rhi::RenderTarget* x = f.pool->acquire(kSmall);
        rhi::RenderTarget* y = f.pool->acquire(kLarge);
        f.pool->release(x);
        rhi::RenderTarget* z = f.pool->acquire(kSmall);
        f.pool->release(y);
        f.pool->release(z);
        (void)f.pool->stats();
    }
    return expect(g_allocations == before,
                  "acquire, release, begin_frame and stats never allocate");
}

} // namespace

int main() {
    try {
        const bool ok = release_reuses_within_a_frame() && release_is_idempotent_and_checked() &&
                        released_target_serves_the_next_frame_at_any_ring_depth() &&
                        unreleased_target_waits_for_its_frame() &&
                        in_flight_target_is_retired_not_destroyed() &&
                        count_cap_refuses_and_counts() && byte_cap_refuses_creation_and_growth() &&
                        idle_targets_are_trimmed() && explicit_trim() &&
                        steady_state_does_not_allocate();
        if (!ok) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "offscreen_pool: unexpected exception: " << error.what() << "\n";
        return 2;
    }
    std::cout << "offscreen_pool: ok\n";
    return 0;
}
