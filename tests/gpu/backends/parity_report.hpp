// SPDX-License-Identifier: MIT
//
// The parity run report and its failure artifacts. The report is the machine-readable half of
// issue #245's "name the class, the counts and the worst region": stable JSON with arm states,
// per-pair verdicts, metrics, difference counts and artifact paths. The visual half is the
// make_diff_image PNG beside it.

#pragma once

#include "parity_policy.hpp"
#include "tessera/harness/image_compare.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::tests::gpu::backends {

enum class ArmState : std::uint8_t {
    Reference,     // the pixel reference of every pair (lavapipe)
    Compared,      // a candidate that was rendered and compared
    StructureOnly, // recorded commands only (null): no pixel output
    Unavailable,   // not built or no device; `detail` names the reason
};

[[nodiscard]] std::string_view to_string(ArmState state) noexcept;

struct ArmReport {
    std::string name;
    ArmState state = ArmState::Unavailable;
    std::string detail;
};

struct PairArtifacts {
    std::string actual;    // written PNG of this arm's render
    std::string reference; // written PNG of the reference render
    std::string diff;      // written make_diff_image PNG; only on failure
};

struct PairReport {
    PairComparison comparison;
    PairArtifacts artifacts;
};

struct SceneReport {
    std::string scene;
    std::vector<PairReport> pairs;
};

// A structure-only arm (null) must record the same command counts as the reference for every
// scene; that is the part of "same output" it can prove without pixels.
struct StructureCheck {
    std::string arm;
    std::string scene;
    std::uint32_t draw_calls = 0;
    std::uint32_t vertices = 0;
    bool matches_reference = true;
};

struct ParityReport {
    std::string reference = "lavapipe";
    std::vector<ArmReport> arms;
    std::vector<SceneReport> scenes;
    std::vector<StructureCheck> structure;
    bool compared = false;
    bool passed = true;
};

[[nodiscard]] std::string parity_report_json(const ParityReport& report);

// The failure artifact bytes: harness::make_diff_image encoded as PNG. Deterministic (stb writes
// no timestamps), so a unit test can decode it back.
[[nodiscard]] std::expected<std::string, harness::ImageError>
diff_png(const harness::RgbaImage& actual, const harness::RgbaImage& expected,
         std::uint32_t per_channel_tolerance);

} // namespace tessera::tests::gpu::backends
