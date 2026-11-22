// SPDX-License-Identifier: MIT
//
// Per-site allocation attribution for `frame_alloc_probe` (issue #701 step 1: "prints the top
// allocation sites"). A count alone says a frame allocates 810 times; it does not say which call to
// fix. The site is the return address of the `operator new` caller, which is one frame up the stack
// and stable for a given call site.
//
// Split out of `frame_alloc_probe.cpp` so that file stays under the 500-line hard limit.
// Header-only and in an anonymous namespace at the include site, so the probe keeps its single
// translation unit (the replaced `operator new` must be the only one in the binary).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <vector>

// `--sites` symbolises a return address with `dladdr`, which is POSIX. On Windows the address is
// printed unresolved, which still distinguishes sites; the probe is a measurement tool, not a gate,
// so a missing symbol name is a cosmetic loss rather than a build break.
#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#define TESSERA_PROBE_HAVE_DLADDR 1
#else
#define TESSERA_PROBE_HAVE_DLADDR 0
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// The caller's return address, which is the one frame that identifies an allocation site without a
// full unwind. GCC/Clang spell it `__builtin_return_address`, MSVC `_ReturnAddress`; both are the
// same fact, and neither is standard C++, so this is the one place that has to know.
#if defined(_MSC_VER)
#define TESSERA_PROBE_RETURN_ADDRESS() _ReturnAddress()
#else
#define TESSERA_PROBE_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace tessera::probe {

// Attribution is off by default: recording a site is far more expensive than the counter itself, so
// the plain measurement path stays allocation-counting only and its numbers are not perturbed.
inline constexpr std::size_t kMaxSites = 512;

struct Site {
    void* address = nullptr;
    std::size_t count = 0;
    std::size_t bytes = 0;
};

inline Site g_sites[kMaxSites];
inline std::size_t g_site_count = 0;
inline bool g_track_sites = false;

inline void record_site(std::size_t size) {
    void* const address = TESSERA_PROBE_RETURN_ADDRESS();
    for (std::size_t i = 0; i < g_site_count; ++i) {
        if (g_sites[i].address == address) {
            ++g_sites[i].count;
            g_sites[i].bytes += size;
            return;
        }
    }
    if (g_site_count < kMaxSites) {
        g_sites[g_site_count++] = Site{address, 1, size};
    }
}

// The top sites, by count. `dladdr` resolves the return address to a symbol; without `-rdynamic` it
// resolves to nothing and the address is printed instead, which is still enough to tell two sites
// apart.
inline void print_sites() {
    std::vector<Site> sorted(g_sites, g_sites + g_site_count);
    std::sort(sorted.begin(), sorted.end(),
              [](const Site& a, const Site& b) { return a.count > b.count; });
    std::printf("\n# top allocation sites (count, bytes, symbol)\n");
    for (const Site& site : sorted) {
        const char* symbol = "?";
#if TESSERA_PROBE_HAVE_DLADDR
        Dl_info info{};
        if (dladdr(site.address, &info) != 0 && info.dli_sname != nullptr) {
            symbol = info.dli_sname;
        }
#endif
        std::printf("%8zu %10zu  %s\n", site.count, site.bytes, symbol);
    }
    std::printf("# %zu distinct site(s)\n", g_site_count);
}

} // namespace tessera::probe
