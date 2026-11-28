// SPDX-License-Identifier: MIT

// ADR-050 open question 6 (docs/design/observable-render-read.md): the `useObservable(svc.member)`
// lowering of the generated base. One `obs::<name>` entry becomes
//
//   - a cached `const Observable<T>*` and its `Subscription` (private fields), resolved in
//     `attach_services()` from the service the entry names;
//   - an `obs_<name>_value()` accessor every read of `obs::<name>` goes through, which returns the
//     live value and the zero value before attach, so `build()` may run before `mount(context)`;
//   - a subscription taken in `mount()` that marks the observable's dirty bit and schedules the
//     component, and released in `unmount()`.
//
// Emission is conditional on `component.observables` being non-empty, so a component without a
// `useObservable` generates not one byte of it (every committed golden stays identical).

#pragma once

#include "codegen_internal.hpp"

#include <string>

namespace tessera::compiler::codegen::internal {

[[nodiscard]] bool uses_observables(const ir::Component& component);

// The include of the observable runtime, for the header's include run.
inline constexpr const char* kObservableInclude = "tessera/appfw/observable.hpp";

// `obs_route_` for `obs::route`: the stem of the generated members.
[[nodiscard]] std::string observable_stem(const ir::Observable& observable);

// The private fields (cached pointer, subscription). Fails when an observable names no declared
// service or has a type with no C++ form.
[[nodiscard]] bool emit_observable_fields(CppWriter& writer, const ir::Component& component,
                                          const ir::IrModule& module, std::string& error);
// `obs_route_value`: the accessor name, for the collision check of the base's member claims.
[[nodiscard]] std::string observable_accessor_name(const ir::Observable& observable);
// The public `obs_<name>_value()` accessors.
[[nodiscard]] bool emit_observable_accessors(CppWriter& writer, const ir::Component& component,
                                             const ir::IrModule& module, std::string& error);
// The lines `attach_services()` ends with: cache each observable's address.
void emit_observable_attach(CppWriter& writer, const ir::Component& component);
// The lines `mount()` ends with (subscribe) and `unmount()` starts with (unsubscribe).
void emit_observable_mount(CppWriter& writer, const ir::Component& component,
                           const ir::IrModule& module);
void emit_observable_unmount(CppWriter& writer, const ir::Component& component);

} // namespace tessera::compiler::codegen::internal
