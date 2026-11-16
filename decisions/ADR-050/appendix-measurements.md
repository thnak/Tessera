# Service-resolve measurements (appendix)

Part of the [ADR-050 decision set](README.md). Section bodies here: `## Appendix: service-resolve measurements`. The heading
numbers, levels and order are the original ones, so `ADR-050 section N` still names the same
heading. Each part is a verbatim slice of the former single file; the shared front matter
(status, date, related decisions, terms) is in the [index](README.md).

## Appendix: service-resolve measurements

Summary of [`service-resolve-benchmark.md`](../../docs/design/service-resolve-benchmark.md) (AMD Ryzen 5
4600H, Windows 11, MSVC 19.51 x64 Release, median of 9 repetitions, two runs agree within about 5%
except where noted there). Microbenchmark limits (single machine and compiler, per-iteration
compiler barrier, perfectly predicted branches, trivial method) are listed in that document.

| Measurement | Result | allocs |
|---|---|---:|
| Use site, cached pointer from mount (B) | 2.1 ns per call | 0 |
| Use site, constructor-injected reference (A) | 2.1 ns per call | 0 |
| Lookup every call, frozen registry of 8 / 64 (B') | 5.3 / 8.0 ns per call | 0 |
| Lookup every call, 1M components out of cache (B' vs B) | 100 vs 68 ns per call | 0 |
| `unordered_map<type_index>` per call, 8 / 64 services (E) | 20 / 40 ns per call | 0 |
| `unordered_map<string>` per call (E) | 66 ns per call | 1 |
| Mount, registry lookup, N = 1 / 8 / 64 services | 7 / 60 / 650 ns per component | 0 |
| Mount, injected pointer copy, N = 1 / 8 / 64 | 0.8 / 3 to 8 / 10 to 66 ns per component | 0 |
| Provide 64 services and freeze | 1.6 us once | 0 |
| Trivial service call: inlined static / CRTP / non-inlined static / virtual | 2.1 / 2.1 / 1.8 / 1.8 ns | 0 |
