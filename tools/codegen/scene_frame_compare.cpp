// SPDX-License-Identifier: MIT

// tessera_scene_frame_compare: the pixel half of the preview canvas gate (issue #167).
//
//   tessera_scene_frame_compare --pair webgpu_vs_lavapipe CANDIDATE.png REFERENCE.png
//
// Exit codes: 0 within tolerance, 1 outside it (or a size difference), 2 usage/setup.
//
// Why a separate binary rather than a Python metric: `tests/golden/tolerance.json` and the C++ gate
// in `src/rendering/golden/` are the repository's one definition of "within tolerance" (issue #245,
// and the cross-backend harness `gpu_backend_parity` applies exactly these calls). A second
// SSIM/deltaE in a script could disagree with that definition, and then two lanes would hold two
// different answers to the same acceptance sentence. So the script reads the *policy* from the one
// JSON file through `scripts/golden_tolerances.py` and the *measurement* from here.
//
// The pair is resolved from the same file, not passed as numbers: a caller that supplied its own
// thresholds could loosen the gate without touching the reviewed policy.

#include "tessera/harness/image_compare.hpp"
#include "tessera/rendering/golden/parity.hpp"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace golden = tessera::rendering::golden;
namespace harness = tessera::harness;

std::string_view kUsage =
    "usage: tessera_scene_frame_compare CANDIDATE.png REFERENCE.png\n"
    "       [--tolerance N] [--max-mismatch R] [--ssim S] [--delta-e D]\n"
    "\n"
    "Compares two RGBA8 PNGs under the cross-backend parity policy\n"
    "(tests/golden/tolerance.json, pair `webgpu_vs_lavapipe` unless the numbers are given).\n"
    "\n"
    "Exit codes: 0 within tolerance; 1 outside it, or a size difference; 2 usage/setup.\n";

int usage_error(std::string_view message) {
    std::cerr << "tessera_scene_frame_compare: " << message << "\n\n" << kUsage;
    return 2;
}

bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    // The policy. The defaults are `webgpu_vs_lavapipe` from tests/golden/tolerance.json, repeated
    // here as plain numbers so this tool has no generated-header dependency; the parity script
    // passes the file's values explicitly, so the file stays the single source of truth and these
    // are only a fallback for a hand-run.
    //
    // `pair_name` owns the text: `ParityTolerance::pair` is a string_view, and pointing it at a
    // temporary made the report print a stray fragment where the pair name belonged. Owning the
    // string here is the fix, and the view is refreshed whenever it changes.
    std::string pair_name = "webgpu_vs_lavapipe";
    golden::ParityTolerance tolerance;
    tolerance.pair = pair_name;
    tolerance.edge_tolerance = 8;
    tolerance.max_mismatch = 0.02;
    tolerance.min_ssim = 0.99;
    tolerance.max_delta_e = 2.0;

    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout << kUsage;
            return 0;
        }
        const bool takes_value = arg == "--tolerance" || arg == "--max-mismatch" ||
                                 arg == "--ssim" || arg == "--delta-e" || arg == "--pair";
        if (takes_value) {
            if (i + 1 >= argc) {
                return usage_error("`" + std::string(arg) + "` needs a value");
            }
            const std::string value(argv[++i]);
            try {
                if (arg == "--tolerance") {
                    tolerance.edge_tolerance = static_cast<std::uint32_t>(std::stoul(value));
                } else if (arg == "--max-mismatch") {
                    tolerance.max_mismatch = std::stod(value);
                } else if (arg == "--ssim") {
                    tolerance.min_ssim = std::stod(value);
                } else if (arg == "--delta-e") {
                    tolerance.max_delta_e = std::stod(value);
                } else {
                    // The pair name is part of the report only; the numbers above carry the policy.
                    pair_name = value;
                    tolerance.pair = pair_name;
                }
            } catch (const std::exception&) {
                return usage_error("`" + std::string(arg) + "` has a malformed value");
            }
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return usage_error("unknown option `" + std::string(arg) + "`");
        }
        inputs.emplace_back(arg);
    }
    if (inputs.size() != 2) {
        return usage_error("two PNG paths are required (candidate then reference)");
    }

    std::vector<std::uint8_t> candidate_bytes;
    std::vector<std::uint8_t> reference_bytes;
    if (!read_file(inputs[0], candidate_bytes)) {
        return usage_error("cannot read `" + inputs[0] + "`");
    }
    if (!read_file(inputs[1], reference_bytes)) {
        return usage_error("cannot read `" + inputs[1] + "`");
    }
    const auto candidate = harness::decode_png(candidate_bytes);
    const auto reference = harness::decode_png(reference_bytes);
    if (!candidate.has_value()) {
        return usage_error("candidate is not a PNG: " + candidate.error().message);
    }
    if (!reference.has_value()) {
        return usage_error("reference is not a PNG: " + reference.error().message);
    }

    const golden::ImageSize size{candidate->width, candidate->height};
    golden::DifferenceClassOptions options;
    options.edge_tolerance = tolerance.edge_tolerance;
    const golden::DifferenceReport differences =
        golden::classify_differences(size, candidate->rgba, size, reference->rgba, options);
    const golden::SsimResult ssim =
        golden::compute_ssim(size, candidate->rgba, size, reference->rgba);
    const golden::DeltaEStats delta_e =
        golden::compute_delta_e(size, candidate->rgba, size, reference->rgba);
    const golden::ParityVerdict verdict =
        golden::evaluate_parity(differences, ssim, delta_e, tolerance);

    // One line per image, in the same field order the other golden tools print, so a log comparison
    // works without a parser.
    std::cout << "compare: pair=" << tolerance.pair << " candidate=" << inputs[0]
              << " reference=" << inputs[1] << "\n";
    std::cout << "compare: size=" << size.width << "x" << size.height
              << " structural=" << differences.counts.structural
              << " edge=" << differences.counts.edge_rounding
              << " mismatch=" << differences.counts.mismatch_ratio() << " ssim=" << ssim.mean
              << " deltaE=" << delta_e.mean << "\n";
    if (!verdict.passed) {
        std::cerr << "compare: FAIL " << verdict.reason << "\n";
        return 1;
    }
    std::cout << "compare: PASS\n";
    return 0;
}
