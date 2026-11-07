#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler {

// A developer-owned block inside generated C++ code.
//
// Format:
//   // [DEVELOPER_LOGIC_START: bind::<id>]
//   ... developer code ...
//   // [DEVELOPER_LOGIC_END: bind::<id>]
struct ProtectedRegion {
    std::string id;
    std::string body;
};

struct MergeResult {
    std::string text;
    std::vector<std::string> warnings;
};

// Returns every protected region in `text`, in file order.
// Throws std::runtime_error on malformed / unterminated / mismatched markers.
std::vector<ProtectedRegion> extract_regions(std::string_view text);

// Injects the developer bodies from `existing` into a freshly generated file.
//
// - Regions present in both files keep the `existing` body.
// - Regions only in `generated` keep the generated body (fresh stub).
// - Regions only in `existing` produce a warning (binding removed/renamed).
//
// Idempotent: merge(merge(g, e), e) == merge(g, e).
MergeResult merge(std::string_view generated, std::string_view existing);

} // namespace tessera::compiler
