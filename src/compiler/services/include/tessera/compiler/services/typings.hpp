// SPDX-License-Identifier: MIT
//
// `services.d.ts` from a parsed `ui/services.json` (docs/specs/services-manifest-v1.md section 3
// item 4): one `interface <Name>` per service with its methods, and a `useService` overload per name,
// so an editor checks the same rules the front end does. Pure text, deterministic, no I/O.
#pragma once

#include "tessera/compiler/services/manifest_types.hpp"

#include <string>
#include <string_view>

namespace tessera::compiler::services {

// The TypeScript spelling of an IR type from the manifest: `int`/`float` -> `number`,
// `string`/`color`/`length` -> `string`, `optional<T>` -> `T | null`, `list<T>` -> `T[]`, a record
// name unchanged, `void` unchanged.
[[nodiscard]] std::string manifest_type_to_ts(std::string_view type);

[[nodiscard]] std::string write_services_typings(const ServicesManifest& manifest);

} // namespace tessera::compiler::services
