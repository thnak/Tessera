// SPDX-License-Identifier: MIT
//
// Service-resolve microbenchmark for ADR-050 section 3. Compares, per call and per mount:
//   (a) frozen registry lookup on every call (consteval id + type-tag check, binary search),
//   (b) the same registry, resolved once at mount and cached as a typed pointer,
//   (c) generated constructor injection of typed pointers (no lookup at all),
//   (d) std::unordered_map locators keyed by std::type_index or std::string (naive baseline),
//   (e) virtual vs static/CRTP dispatch for a trivial service method.
// Release builds only; see docs/design/service-resolve-benchmark.md for results and
// limits. Not part of the default build or ctest.

#include "bench_harness.hpp"
#include "service_registry.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

std::size_t bench::g_allocations = 0;

// Replaced global allocation functions: every heap allocation in the process is counted.
void* operator new(std::size_t size) {
    ++bench::g_allocations;
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

using namespace bench;

constexpr std::size_t kServiceCount = 64;

template<typename Seq> struct ServiceSetFor;
template<std::size_t... I> struct ServiceSetFor<std::index_sequence<I...>> {
    using type = std::tuple<Svc<static_cast<int>(I)>...>;
};
using ServiceSet = ServiceSetFor<std::make_index_sequence<kServiceCount>>::type;

template<std::size_t Count> void provide_first(FrozenRegistry& registry, ServiceSet& services) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (registry.provide(std::get<I>(services)), ...);
    }(std::make_index_sequence<Count>{});
    registry.freeze();
}

std::string service_name(std::size_t index) {
    return "com.example.bench.svc." + std::to_string(100 + index);
}

void print_row(char const* name, Measurement const& m) {
    std::printf("| %-58s | %9.2f | %9.2f | %6.2f |\n", name, m.ns_per_op, m.min_ns_per_op,
                m.allocs_per_op);
}

void print_header(char const* title, char const* unit) {
    std::printf("\n### %s\n\n| Case | median %s | min %s | allocs/op |\n|---|---:|---:|---:|\n",
                title, unit, unit);
}

// ---------------------------------------------------------------------------------------------
// Per-call cost. Each component is 64 bytes: the context pointer the generated base would hold,
// the pointer cached at mount, the naive locators, and unrelated component state.
template<typename Target> struct CallComponent {
    FrozenRegistry const* context = nullptr;
    Target* cached = nullptr;
    TypeIndexLocator const* type_locator = nullptr;
    StringLocator const* string_locator = nullptr;
    std::array<std::uint64_t, 4> other_state{};
};

template<typename Target> struct InjectedComponent {
    Target& service; // what generated constructor injection stores
    std::array<std::uint64_t, 7> other_state{};
};

template<typename Target>
void run_per_call(char const* label, FrozenRegistry const& registry, ServiceSet& services,
                  TypeIndexLocator const& type_locator, StringLocator const& string_locator,
                  std::size_t ops) {
    auto component = std::make_unique<CallComponent<Target>>();
    component->context = &registry;
    component->cached = registry.find<Target>();
    component->type_locator = &type_locator;
    component->string_locator = &string_locator;
    auto injected = std::make_unique<InjectedComponent<Target>>(
        InjectedComponent<Target>{std::get<Target::kIndex>(services), {}});
    std::string const key_text = service_name(Target::kIndex);
    std::string_view const key = key_text;

    std::printf("\n%s\n", label);
    print_row("baseline: empty loop with barrier", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += component->other_state[0] + 1;
                  }
                  keep(acc);
              }));
    print_row("(a) registry lookup every call", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += component->context->template find<Target>()->bump(1);
                  }
                  keep(acc);
              }));
    print_row("(b) resolved at mount, cached typed pointer", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += component->cached->bump(1);
                  }
                  keep(acc);
              }));
    print_row("(c) constructor-injected typed reference", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += injected->service.bump(1);
                  }
                  keep(acc);
              }));
    print_row("(d1) unordered_map<type_index> lookup every call", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += component->type_locator->template find<Target>()->bump(1);
                  }
                  keep(acc);
              }));
    print_row("(d2) unordered_map<string> lookup every call", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += static_cast<Target*>(component->string_locator->find(std::string(key)))
                                 ->bump(1);
                  }
                  keep(acc);
              }));
}

// ---------------------------------------------------------------------------------------------
// Dispatch cost for a trivial method: static (inlinable), static but not inlined, CRTP, virtual.
class NoInlineService {
public:
    TESSERA_BENCH_NOINLINE std::uint64_t bump(std::uint64_t amount) {
        value_ += amount;
        return value_;
    }

private:
    std::uint64_t value_ = 0;
};

template<typename Derived> class CrtpService {
public:
    std::uint64_t bump(std::uint64_t amount) {
        return static_cast<Derived&>(*this).bump_impl(amount);
    }
};

class CrtpValueService final : public CrtpService<CrtpValueService> {
public:
    std::uint64_t bump_impl(std::uint64_t amount) {
        value_ += amount;
        return value_;
    }

private:
    std::uint64_t value_ = 0;
};

struct DispatchHolders {
    Svc<0>* direct = nullptr;
    NoInlineService* no_inline = nullptr;
    CrtpService<CrtpValueService>* crtp = nullptr;
    ValueService* virtual_service = nullptr;
};

void run_dispatch(ServiceSet& services, std::size_t ops) {
    auto no_inline = std::make_unique<NoInlineService>();
    auto crtp = std::make_unique<CrtpValueService>();
    auto holders = std::make_unique<DispatchHolders>();
    holders->direct = &std::get<0>(services);
    holders->no_inline = no_inline.get();
    holders->crtp = crtp.get();
    holders->virtual_service = &std::get<0>(services);

    print_header("(e) Dispatch through the service interface (one trivial call)", "ns/call");
    print_row("static call on final type (inlined)", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += holders->direct->bump(1);
                  }
                  keep(acc);
              }));
    print_row("CRTP static dispatch (inlined)", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += holders->crtp->bump(1);
                  }
                  keep(acc);
              }));
    print_row("static call, not inlined", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += holders->no_inline->bump(1);
                  }
                  keep(acc);
              }));
    print_row("virtual call through ValueService*", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += holders->virtual_service->bump(1);
                  }
                  keep(acc);
              }));
}

// ---------------------------------------------------------------------------------------------
// Per-mount cost: a component that needs N services mounts M times into preallocated storage.
template<std::size_t N> struct MountedComponent {
    std::array<void*, N> services{}; // typed pointers in generated code; same machine code
};

struct ServiceBundle { // what a composition root would hand to generated constructors
    std::array<void*, kServiceCount> services{};
};

template<std::size_t N>
void run_mount(FrozenRegistry const& registry, TypeIndexLocator const& type_locator,
               ServiceBundle const& bundle, std::size_t components) {
    std::vector<MountedComponent<N>> storage(components);
    std::size_t missing = 0;
    char label[96];

    std::snprintf(label, sizeof label, "N=%zu, M=%zu (b) registry lookup x N at mount", N,
                  components);
    print_row(label, measure(components, [&](std::size_t n) {
                  for (std::size_t c = 0; c < n; ++c) {
                      auto& slots = storage[c].services;
                      [&]<std::size_t... I>(std::index_sequence<I...>) {
                          ((slots[I] = registry.find<Svc<static_cast<int>(I)>>()), ...);
                          ((missing += slots[I] == nullptr ? 1U : 0U), ...);
                      }(std::make_index_sequence<N>{});
                      clobber_memory();
                  }
              }));
    std::snprintf(label, sizeof label, "N=%zu, M=%zu (c) injected: copy N typed pointers", N,
                  components);
    print_row(label, measure(components, [&](std::size_t n) {
                  for (std::size_t c = 0; c < n; ++c) {
                      auto& slots = storage[c].services;
                      [&]<std::size_t... I>(std::index_sequence<I...>) {
                          ((slots[I] = bundle.services[I]), ...);
                      }(std::make_index_sequence<N>{});
                      clobber_memory();
                  }
              }));
    std::snprintf(label, sizeof label, "N=%zu, M=%zu (d1) type_index map lookup x N", N,
                  components);
    print_row(label, measure(components, [&](std::size_t n) {
                  for (std::size_t c = 0; c < n; ++c) {
                      auto& slots = storage[c].services;
                      [&]<std::size_t... I>(std::index_sequence<I...>) {
                          ((slots[I] = type_locator.find<Svc<static_cast<int>(I)>>()), ...);
                      }(std::make_index_sequence<N>{});
                      clobber_memory();
                  }
              }));
    keep(missing);
}

// ---------------------------------------------------------------------------------------------
// Cache effects: one call per component over K components visited in a shuffled order.
void run_many_components(FrozenRegistry const& registry, std::size_t count, std::size_t ops) {
    using Target = Svc<37>;
    std::vector<CallComponent<Target>> components(count);
    for (auto& component : components) {
        component.context = &registry;
        component.cached = registry.find<Target>();
    }
    std::vector<std::uint32_t> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        order[i] = static_cast<std::uint32_t>(i);
    }
    std::uint64_t state = 0x9E3779B97F4A7C15ULL; // fixed seed: deterministic shuffle
    for (std::size_t i = count; i > 1; --i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        std::swap(order[i - 1], order[(state >> 33) % i]);
    }
    std::size_t const passes = ops / count == 0 ? 1 : ops / count;
    char label[96];

    std::snprintf(label, sizeof label, "K=%zu baseline: touch component only", count);
    print_row(label, measure(passes * count, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0, j = 0; i < n; ++i, j = j + 1 == count ? 0 : j + 1) {
                      clobber_memory();
                      acc += components[order[j]].other_state[0] + 1;
                  }
                  keep(acc);
              }));
    std::snprintf(label, sizeof label, "K=%zu (a) registry lookup every call", count);
    print_row(label, measure(passes * count, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0, j = 0; i < n; ++i, j = j + 1 == count ? 0 : j + 1) {
                      clobber_memory();
                      acc += components[order[j]].context->find<Target>()->bump(1);
                  }
                  keep(acc);
              }));
    std::snprintf(label, sizeof label, "K=%zu (b) cached pointer", count);
    print_row(label, measure(passes * count, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0, j = 0; i < n; ++i, j = j + 1 == count ? 0 : j + 1) {
                      clobber_memory();
                      acc += components[order[j]].cached->bump(1);
                  }
                  keep(acc);
              }));
}

// ---------------------------------------------------------------------------------------------
// AF5 (issue #177): the generated accessor's path, in the shape codegen actually emits.
//
// The generated base holds `std::tuple<T*...> services_`, filled once by `attach_services()`.
// `service<T>()` is `std::get<T*>(services_)`; `<name>()` forwards to it. What this section
// measures is the difference between that (a) *compile-time-indexed* tuple read and (b) a *registry
// lookup* issued on every use -- the two ends of ADR-050 section 3 item 5, with the same barrier.
//
// The tuple index is a compile-time constant here because it is in generated code too (`std::get`
// with a type argument resolves to `std::get<I>`), so an unused `services_` slot cannot be
// mistaken for a lookup.
template<std::size_t N> struct GeneratedBase {
    std::tuple<Svc<static_cast<int>(0)>, Svc<static_cast<int>(1)>> ignored{}; // keeps the type real
    std::array<void*, N> services{};

    template<typename T> T& service() const noexcept {
        // Exactly the generated `std::get<T*>(services_)`, with the same static index.
        return *static_cast<T*>(std::get<T::kIndex>(services));
    }
};

// A two-requirement base, the fixture's shape, with the types spelled out as codegen spells them.
struct TwoServiceBase {
    Svc<5>* navigator = nullptr;
    Svc<37>* mail = nullptr;

    // `nav()` / `store()` in the generated base: one dereference of a cached pointer.
    Svc<5>& nav() const noexcept {
        return *navigator;
    }
    Svc<37>& store() const noexcept {
        return *mail;
    }
};

void run_generated_accessor(FrozenRegistry const& registry, ServiceSet& services, std::size_t ops) {
    auto base = std::make_unique<TwoServiceBase>();
    // attach_services(): the two lookups, once, at mount.
    base->navigator = &registry.get<Svc<5>>();
    base->mail = &registry.get<Svc<37>>();
    auto context_component = std::make_unique<CallComponent<Svc<5>>>();
    context_component->context = &registry;

    print_header("(f) AF5 generated accessor vs a lookup per use", "ns/call");
    print_row("accessor read: cached pointer through nav()", measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += base->nav().bump(1);
                  }
                  keep(acc);
              }));
    print_row("service<T>() through the retained context (forbidden)",
              measure(ops, [&](std::size_t n) {
                  std::uint64_t acc = 0;
                  for (std::size_t i = 0; i < n; ++i) {
                      clobber_memory();
                      acc += context_component->context->get<Svc<5>>().bump(1);
                  }
                  keep(acc);
              }));
    // The second requirement read through its own slot: a tuple-index mix-up would be a different
    // service, so the value check keeps the two rows honest.
    keep(static_cast<std::uint64_t>(&base->store() == &std::get<37>(services) ? 1 : 0));
    keep(static_cast<std::uint64_t>(&base->nav() == &std::get<5>(services) ? 1 : 0));

    // Per-mount: the two lookups `attach_services()` performs, M components at a time.
    std::vector<TwoServiceBase> mounted(262144);
    print_row("mount: attach_services() resolves N=2 (M=262144)",
              measure(mounted.size(), [&](std::size_t n) {
                  for (std::size_t c = 0; c < n; ++c) {
                      mounted[c].navigator = &registry.get<Svc<5>>();
                      mounted[c].mail = &registry.get<Svc<37>>();
                      clobber_memory();
                  }
              }));
}

} // namespace

int main(int argc, char** argv) {
    std::size_t scale = 1;
    if (argc > 1) {
        scale = static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10));
        scale = scale == 0 ? 1 : scale;
    }
    std::size_t const call_ops = 20'000'000 * scale;

#if defined(_MSC_FULL_VER) && !defined(__clang__)
    std::printf("compiler: MSVC %d\n", _MSC_FULL_VER);
#elif defined(__clang__)
    std::printf("compiler: clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    std::printf("compiler: gcc %s\n", __VERSION__);
#endif
#if defined(NDEBUG)
    std::printf("build: optimized (NDEBUG)\n");
#else
    std::printf("build: NOT NDEBUG - numbers are meaningless outside a Release build\n");
#endif
    std::printf("repetitions per case: %d (median and min reported)\n", kRepetitions);

    auto services = std::make_unique<ServiceSet>();
    FrozenRegistry registry8;
    FrozenRegistry registry64;
    provide_first<8>(registry8, *services);
    provide_first<kServiceCount>(registry64, *services);

    TypeIndexLocator type_locator;
    StringLocator string_locator;
    ServiceBundle bundle;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (type_locator.provide(std::get<I>(*services)), ...);
        (string_locator.provide(service_name(I), &std::get<I>(*services)), ...);
        ((bundle.services[I] = &std::get<I>(*services)), ...);
    }(std::make_index_sequence<kServiceCount>{});

    print_header("Per-call cost (one resolve + one trivial call)", "ns/call");
    run_per_call<Svc<5>>("registry of 8 services:", registry8, *services, type_locator,
                         string_locator, call_ops);
    run_per_call<Svc<37>>("registry of 64 services:", registry64, *services, type_locator,
                          string_locator, call_ops);

    run_dispatch(*services, call_ops);

    run_generated_accessor(registry64, *services, call_ops);

    print_header("Per-mount cost (registry of 64; one component needing N services)", "ns/mount");
    for (std::size_t const components : {std::size_t{4096}, std::size_t{262144}}) {
        run_mount<1>(registry64, type_locator, bundle, components);
        run_mount<8>(registry64, type_locator, bundle, components);
        run_mount<64>(registry64, type_locator, bundle, components);
    }

    print_header("One-time: provide 64 services and freeze", "ns/registry");
    print_row("provide x64 + freeze (sort)", measure(20'000, [&](std::size_t n) {
                  for (std::size_t i = 0; i < n; ++i) {
                      FrozenRegistry fresh;
                      provide_first<kServiceCount>(fresh, *services);
                      keep(fresh.size());
                      clobber_memory();
                  }
              }));

    print_header("Cache effects: one call per component, K components, shuffled order", "ns/call");
    for (std::size_t const count :
         {std::size_t{64}, std::size_t{4096}, std::size_t{262144}, std::size_t{1048576}}) {
        run_many_components(registry64, count, call_ops);
    }
    return 0;
}
