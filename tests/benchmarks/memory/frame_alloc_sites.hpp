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
#include <cstring>
#include <vector>

// `--sites` symbolises a return address with `dladdr`, which is POSIX. On Windows the address is
// printed unresolved, which still distinguishes sites; the probe is a measurement tool, not a gate,
// so a missing symbol name is a cosmetic loss rather than a build break.
#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#define TESSERA_PROBE_HAVE_DLADDR 1
#if defined(__linux__)
#include <execinfo.h>
#define TESSERA_PROBE_HAVE_BACKTRACE 1
#endif
#else
#define TESSERA_PROBE_HAVE_DLADDR 0
#endif
#ifndef TESSERA_PROBE_HAVE_BACKTRACE
#define TESSERA_PROBE_HAVE_BACKTRACE 0
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// Windows: CaptureStackBackTrace for the chain and DbgHelp for the names (the PDB sits beside the
// executable). `<windows.h>` must come first.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// clang-format off
#include <dbghelp.h>
// clang-format on
#pragma comment(lib, "dbghelp.lib")
#define TESSERA_PROBE_HAVE_WIN_STACK 1
#else
#define TESSERA_PROBE_HAVE_WIN_STACK 0
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
// A site is the call chain above `operator new`, not one return address: the immediate caller of
// `operator new` is almost always a library template (`std::vector::push_back`, `std::string`),
// which names nothing the engine owns. Four frames reach the engine function that asked.
inline constexpr std::size_t kSiteDepth = 5;
inline constexpr int kSkipFrames = 3; // record_site, count, operator new

struct Site {
    void* frames[kSiteDepth] = {};
    std::size_t count = 0;
    std::size_t bytes = 0;
};

inline Site g_sites[kMaxSites];
inline std::size_t g_site_count = 0;
inline bool g_track_sites = false;

inline void record_site(std::size_t size) {
    void* chain[kSiteDepth] = {};
#if TESSERA_PROBE_HAVE_BACKTRACE
    void* raw[kSiteDepth + kSkipFrames] = {};
    const int found = backtrace(raw, static_cast<int>(kSiteDepth) + kSkipFrames);
    for (int i = kSkipFrames; i < found; ++i) {
        chain[i - kSkipFrames] = raw[i];
    }
#elif TESSERA_PROBE_HAVE_WIN_STACK
    void* raw[kSiteDepth + kSkipFrames] = {};
    const USHORT found = CaptureStackBackTrace(0, static_cast<DWORD>(kSiteDepth) + kSkipFrames, raw,
                                               nullptr);
    for (int i = kSkipFrames; i < static_cast<int>(found); ++i) {
        chain[i - kSkipFrames] = raw[i];
    }
#else
    chain[0] = TESSERA_PROBE_RETURN_ADDRESS();
#endif
    for (std::size_t i = 0; i < g_site_count; ++i) {
        if (std::memcmp(g_sites[i].frames, chain, sizeof(chain)) == 0) {
            ++g_sites[i].count;
            g_sites[i].bytes += size;
            return;
        }
    }
    if (g_site_count < kMaxSites) {
        Site& site = g_sites[g_site_count++];
        std::memcpy(site.frames, chain, sizeof(chain));
        site.count = 1;
        site.bytes = size;
    }
}

// The top sites, by count. Each is printed as the symbol (when `dladdr` finds one) and the offset
// from the module base, so `addr2line -f -C -e <binary> <offset>...` names every frame of the chain
// even in a binary linked without `-rdynamic`.
inline void print_sites() {
    std::vector<Site> sorted(g_sites, g_sites + g_site_count);
    std::sort(sorted.begin(), sorted.end(),
              [](const Site& a, const Site& b) { return a.count > b.count; });
    std::printf("\n# top allocation sites (count, bytes, frame offsets from the module base)\n");
    for (const Site& site : sorted) {
        std::printf("%8zu %10zu ", site.count, site.bytes);
        for (void* frame : site.frames) {
            if (frame == nullptr) {
                break;
            }
#if TESSERA_PROBE_HAVE_WIN_STACK
            {
                static const bool initialised = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != 0;
                alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 displacement = 0;
                if (initialised &&
                    SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(frame), &displacement,
                                symbol) != 0) {
                    std::printf(" %.80s", symbol->Name);
                    continue;
                }
            }
#endif
#if TESSERA_PROBE_HAVE_DLADDR
            Dl_info info{};
            if (dladdr(frame, &info) != 0 && info.dli_fbase != nullptr) {
                std::printf(" 0x%zx", static_cast<std::size_t>(static_cast<char*>(frame) -
                                                                static_cast<char*>(info.dli_fbase)));
                continue;
            }
#endif
            std::printf(" %p", frame);
        }
        std::printf("\n");
    }
    std::printf("# %zu distinct site(s)\n", g_site_count);
}

} // namespace tessera::probe
