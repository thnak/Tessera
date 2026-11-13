// SPDX-License-Identifier: MIT

#include "parity_report.hpp"

#include <cstddef>
#include <string>

namespace tessera::tests::gpu::backends {
namespace {

void append_json_string(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[(static_cast<unsigned char>(character) >> 4U) & 0xFU]);
                out.push_back(kHex[static_cast<unsigned char>(character) & 0xFU]);
            } else {
                out.push_back(character);
            }
        }
    }
    out.push_back('"');
}

void append_number(std::string& out, double value) {
    out += std::to_string(value);
}

void append_worst_region(std::string& out, const rendering::golden::DifferenceReport& differences) {
    out += "\"worst\":{\"class\":";
    append_json_string(out, rendering::golden::to_string(differences.worst_class));
    out += ",\"x\":" + std::to_string(differences.worst_region.x) +
           ",\"y\":" + std::to_string(differences.worst_region.y) +
           ",\"width\":" + std::to_string(differences.worst_region.width) +
           ",\"height\":" + std::to_string(differences.worst_region.height) +
           ",\"pixels\":" + std::to_string(differences.worst_region.pixels) + "}";
}

void append_pair(std::string& out, const PairReport& pair) {
    const PairComparison& comparison = pair.comparison;
    out += "{\"pair\":";
    append_json_string(out, comparison.pair);
    out += ",\"status\":";
    append_json_string(out, comparison.passed ? "pass" : "fail");
    out += ",\"reason\":";
    append_json_string(out, comparison.reason);

    out +=
        ",\"policy\":{\"edge_tolerance\":" + std::to_string(comparison.tolerance.edge_tolerance) +
        ",\"max_mismatch\":";
    append_number(out, comparison.tolerance.max_mismatch);
    out += ",\"min_ssim\":";
    append_number(out, comparison.tolerance.min_ssim);
    out += ",\"max_delta_e\":";
    append_number(out, comparison.tolerance.max_delta_e);
    out.push_back('}');

    out += ",\"metrics\":{\"ssim\":";
    append_number(out, comparison.ssim.mean);
    out += ",\"ssim_worst\":";
    append_number(out, comparison.ssim.worst);
    out += ",\"delta_e_mean\":";
    append_number(out, comparison.delta_e.mean);
    out += ",\"delta_e_max\":";
    append_number(out, comparison.delta_e.max);
    out.push_back('}');

    out += ",\"counts\":{\"pixels\":" + std::to_string(comparison.differences.counts.pixels) +
           ",\"mismatched\":" + std::to_string(comparison.differences.counts.mismatched) +
           ",\"edge_rounding\":" + std::to_string(comparison.differences.counts.edge_rounding) +
           ",\"structural\":" + std::to_string(comparison.differences.counts.structural) + "},";
    append_worst_region(out, comparison.differences);

    out += ",\"artifacts\":{\"actual\":";
    append_json_string(out, pair.artifacts.actual);
    out += ",\"reference\":";
    append_json_string(out, pair.artifacts.reference);
    out += ",\"diff\":";
    append_json_string(out, pair.artifacts.diff);
    out += "}}";
}

} // namespace

std::string_view to_string(ArmState state) noexcept {
    switch (state) {
    case ArmState::Reference:
        return "reference";
    case ArmState::Compared:
        return "compared";
    case ArmState::StructureOnly:
        return "structure-only";
    case ArmState::Unavailable:
        return "unavailable";
    }
    return "unavailable";
}

std::string parity_report_json(const ParityReport& report) {
    std::string out = "{\"reportVersion\":1,\"reference\":";
    append_json_string(out, report.reference);
    out += ",\"compared\":";
    out += report.compared ? "true" : "false";
    out += ",\"verdict\":";
    append_json_string(out, report.passed ? "pass" : "fail");

    out += ",\"arms\":[";
    for (std::size_t index = 0; index < report.arms.size(); ++index) {
        if (index > 0) {
            out.push_back(',');
        }
        const ArmReport& arm = report.arms[index];
        out += "{\"name\":";
        append_json_string(out, arm.name);
        out += ",\"state\":";
        append_json_string(out, to_string(arm.state));
        out += ",\"detail\":";
        append_json_string(out, arm.detail);
        out.push_back('}');
    }

    out += "],\"scenes\":[";
    for (std::size_t scene_index = 0; scene_index < report.scenes.size(); ++scene_index) {
        if (scene_index > 0) {
            out.push_back(',');
        }
        const SceneReport& scene = report.scenes[scene_index];
        out += "{\"scene\":";
        append_json_string(out, scene.scene);
        out += ",\"pairs\":[";
        for (std::size_t pair_index = 0; pair_index < scene.pairs.size(); ++pair_index) {
            if (pair_index > 0) {
                out.push_back(',');
            }
            append_pair(out, scene.pairs[pair_index]);
        }
        out += "]}";
    }

    out += "],\"structure\":[";
    for (std::size_t index = 0; index < report.structure.size(); ++index) {
        if (index > 0) {
            out.push_back(',');
        }
        const StructureCheck& check = report.structure[index];
        out += "{\"arm\":";
        append_json_string(out, check.arm);
        out += ",\"scene\":";
        append_json_string(out, check.scene);
        out += ",\"draw_calls\":" + std::to_string(check.draw_calls) +
               ",\"vertices\":" + std::to_string(check.vertices) + ",\"matches_reference\":";
        out += check.matches_reference ? "true" : "false";
        out.push_back('}');
    }
    out += "]}";
    return out;
}

std::expected<std::string, harness::ImageError> diff_png(const harness::RgbaImage& actual,
                                                         const harness::RgbaImage& expected,
                                                         std::uint32_t per_channel_tolerance) {
    std::expected<harness::RgbaImage, harness::ImageError> diff =
        harness::make_diff_image(actual, expected, per_channel_tolerance);
    if (!diff.has_value()) {
        return std::unexpected(diff.error());
    }
    return harness::encode_png(*diff);
}

} // namespace tessera::tests::gpu::backends
