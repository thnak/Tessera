// SPDX-License-Identifier: MIT

#pragma once

#include <string>

// The two outputs of the schema dump tool (docs/specs/jsx-intrinsics-v1/machine-readable.md
// section 4): `intrinsics.json` and `tessera.d.ts`. Shared by the CLI and the golden test so both
// write the same bytes.
namespace tessera::schema::dump {

[[nodiscard]] std::string json();
[[nodiscard]] std::string dts();

} // namespace tessera::schema::dump
