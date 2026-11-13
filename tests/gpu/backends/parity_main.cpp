// SPDX-License-Identifier: MIT
//
// Cross-backend image parity harness (issue #245).
//
//   gpu_backend_parity --out-dir <dir>                 render every available arm and compare
//   gpu_backend_parity --out-dir <dir> --write-reference  render the lavapipe reference images
//
// The compare mode renders the fixed scene set (parity_scenes.hpp) through the RHI on every arm
// the build and the host can provide. The lavapipe software Vulkan driver is the reference
// (ADR-009); every other pixel-producing arm is compared against it through the per-pair policy in
// tests/golden/tolerance.json (SSIM on luma, CIE76 deltaE, edge-rounding vs structural classes).
// An arm that is not built or has no device prints a "skipped" line naming the reason - never a
// silent pass. The null backend has no image output, so it only proves that the same scene records
// the same command counts as the reference.
//
// On a failure the harness writes the make_diff_image PNG and the JSON report into the output
// directory and prints both paths. Exit codes: 0 when every comparison passed (or no comparison
// was possible), 1 on a parity/determinism/structure failure, 2 on a usage or I/O error.

#include "parity_backend.hpp"
#include "parity_policy.hpp"
#include "parity_render.hpp"
#include "parity_report.hpp"
#include "parity_scenes.hpp"
#include "tessera/harness/image_compare.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace tessera::tests::gpu::backends;
using tessera::harness::ImageError;
using tessera::harness::RgbaImage;

struct Options {
    std::filesystem::path out_dir;
    bool write_reference = false;
    bool help = false;
};

void print_usage(std::ostream& out) {
    out << "usage: gpu_backend_parity --out-dir <dir> [--write-reference]\n"
           "  --out-dir          directory for actual/reference/diff artifacts and the report\n"
           "  --write-reference  render the lavapipe reference images (needs the lvp ICD)\n"
           "  --help             print this text\n";
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            return true;
        }
        if (argument == "--write-reference") {
            options.write_reference = true;
            continue;
        }
        if (argument == "--out-dir") {
            if (index + 1 >= argc) {
                std::cerr << "gpu_backend_parity: --out-dir needs a path\n";
                return false;
            }
            options.out_dir = argv[++index];
            continue;
        }
        std::cerr << "gpu_backend_parity: unknown argument '" << argument << "'\n";
        print_usage(std::cerr);
        return false;
    }
    if (!options.help && options.out_dir.empty()) {
        std::cerr << "gpu_backend_parity: --out-dir is required\n";
        print_usage(std::cerr);
        return false;
    }
    return true;
}

bool write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

std::optional<std::string> read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof()) {
        return std::nullopt;
    }
    return bytes;
}

std::string display_path(const std::filesystem::path& path) {
    return path.generic_string();
}

// One opened arm with its rendered scene set (empty for structure-only arms in the pixel sense).
struct ArmRun {
    OpenedArm opened;
    std::unique_ptr<tessera::rendering::rhi::Pipeline> pipeline;
    std::vector<RenderedScene> scenes;
    bool deterministic = true;
};

std::vector<std::filesystem::path> scene_png_paths(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> paths;
    for (const Scene& scene : parity_scenes()) {
        paths.push_back(root / (std::string(scene.name) + ".png"));
    }
    return paths;
}

int run_reference(const Options& options) {
    OpenedArm arm = open_arm(Arm::Lavapipe);
    if (!arm.available()) {
        std::cout << "gpu_backend_parity: reference skipped (" << arm.unavailable_reason << ")\n";
        return 0;
    }
    std::unique_ptr<tessera::rendering::rhi::Pipeline> pipeline(
        arm.device->create_pipeline(pipeline_for(arm)));
    if (pipeline == nullptr) {
        std::cerr << "gpu_backend_parity: cannot create the batched2d pipeline\n";
        return 2;
    }

    const std::filesystem::path reference_dir = options.out_dir / "reference";
    std::error_code error;
    std::filesystem::create_directories(reference_dir, error);
    if (error) {
        std::cerr << "gpu_backend_parity: cannot create '" << display_path(reference_dir)
                  << "': " << error.message() << "\n";
        return 2;
    }

    const std::vector<std::filesystem::path> paths = scene_png_paths(reference_dir);
    const std::span<const Scene> scenes = parity_scenes();
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        const RenderedScene rendered = render_scene(*arm.device, *pipeline, scenes[index].name);
        const RgbaImage image{rendered.width, rendered.height, rendered.rgba};
        const std::expected<std::string, ImageError> encoded = tessera::harness::encode_png(image);
        if (!encoded.has_value() || !write_bytes(paths[index], *encoded)) {
            std::cerr << "gpu_backend_parity: cannot write '" << display_path(paths[index])
                      << "'\n";
            return 2;
        }
        std::cout << "gpu_backend_parity: reference " << scenes[index].name << ": "
                  << display_path(paths[index]) << "\n";
    }
    std::cout << "gpu_backend_parity: reference written to " << display_path(reference_dir) << " ("
              << scenes.size() << " scenes)\n";
    return 0;
}

int run_compare(const Options& options) {
    const std::filesystem::path actual_root = options.out_dir / "actual";
    const std::filesystem::path reference_dir = options.out_dir / "reference";
    const std::filesystem::path diff_root = options.out_dir / "diff";
    std::error_code error;
    std::filesystem::create_directories(actual_root, error);
    if (error) {
        std::cerr << "gpu_backend_parity: cannot create '" << display_path(actual_root)
                  << "': " << error.message() << "\n";
        return 2;
    }

    ParityReport report;
    std::vector<ArmRun> runs;
    std::vector<RenderedScene> structure_baseline;
    std::string structure_baseline_arm;
    bool deterministic = true;
    bool render_failed = false;

    for (const Arm arm : parity_arms()) {
        ArmRun run;
        run.opened = open_arm(arm);
        const std::string name = run.opened.name;
        if (!run.opened.available()) {
            report.arms.push_back(
                ArmReport{name, ArmState::Unavailable, run.opened.unavailable_reason});
            std::cout << "gpu_backend_parity: backend '" << name << "': skipped ("
                      << run.opened.unavailable_reason << ")\n";
            continue;
        }
        run.pipeline = std::unique_ptr<tessera::rendering::rhi::Pipeline>(
            run.opened.device->create_pipeline(pipeline_for(run.opened)));
        if (run.pipeline == nullptr) {
            std::cerr << "gpu_backend_parity: backend '" << name
                      << "': cannot create the batched2d pipeline\n";
            render_failed = true;
            continue;
        }

        const bool pixel_arm = !run.opened.structure_only;
        const ArmState state =
            !pixel_arm ? ArmState::StructureOnly
                       : (arm == Arm::Lavapipe ? ArmState::Reference : ArmState::Compared);
        report.arms.push_back(ArmReport{name, state, ""});
        const std::span<const Scene> scenes = parity_scenes();
        try {
            if (pixel_arm) {
                const std::filesystem::path arm_dir = actual_root / name;
                std::filesystem::create_directories(arm_dir, error);
                if (error) {
                    std::cerr << "gpu_backend_parity: cannot create '" << display_path(arm_dir)
                              << "': " << error.message() << "\n";
                    return 2;
                }
                const std::vector<std::filesystem::path> paths = scene_png_paths(arm_dir);
                for (std::size_t index = 0; index < scenes.size(); ++index) {
                    RenderedScene rendered =
                        render_scene(*run.opened.device, *run.pipeline, scenes[index].name);
                    const RenderedScene repeat =
                        render_scene(*run.opened.device, *run.pipeline, scenes[index].name);
                    if (rendered.rgba != repeat.rgba) {
                        run.deterministic = false;
                        deterministic = false;
                        std::cout << "gpu_backend_parity: backend '" << name << "' scene '"
                                  << scenes[index].name
                                  << "': two runs differ (non-deterministic render)\n";
                    }
                    const RgbaImage image{rendered.width, rendered.height, rendered.rgba};
                    const std::expected<std::string, ImageError> encoded =
                        tessera::harness::encode_png(image);
                    if (!encoded.has_value() || !write_bytes(paths[index], *encoded)) {
                        std::cerr << "gpu_backend_parity: cannot write '"
                                  << display_path(paths[index]) << "'\n";
                        return 2;
                    }
                    run.scenes.push_back(std::move(rendered));
                }
                std::cout << "gpu_backend_parity: backend '" << name << "': available ("
                          << scenes.size()
                          << " scenes, deterministic=" << (run.deterministic ? "yes" : "no")
                          << ")\n";
            } else {
                for (const Scene& scene : scenes) {
                    run.scenes.push_back(
                        render_scene(*run.opened.device, *run.pipeline, scene.name));
                }
                std::cout << "gpu_backend_parity: backend '" << name
                          << "': structure-only (no pixel output; command counts checked)\n";
            }
        } catch (const std::exception& exception) {
            std::cerr << "gpu_backend_parity: backend '" << name << "': " << exception.what()
                      << "\n";
            render_failed = true;
            continue;
        }
        if (structure_baseline.empty() && pixel_arm) {
            structure_baseline = run.scenes;
            structure_baseline_arm = name;
        }
        runs.push_back(std::move(run));
    }

    // Structure-only arms must record the same command counts as the pixel baseline.
    for (const ArmRun& run : runs) {
        if (!run.opened.structure_only) {
            continue;
        }
        for (std::size_t index = 0; index < run.scenes.size(); ++index) {
            StructureCheck check;
            check.arm = run.opened.name;
            check.scene = std::string(parity_scenes()[index].name);
            check.draw_calls = run.scenes[index].draw_calls;
            check.vertices = run.scenes[index].vertices;
            if (!structure_baseline.empty()) {
                check.matches_reference =
                    structure_baseline[index].draw_calls == check.draw_calls &&
                    structure_baseline[index].vertices == check.vertices;
                if (!check.matches_reference) {
                    report.passed = false;
                    std::cout << "gpu_backend_parity: FAIL structure " << check.arm << " vs "
                              << structure_baseline_arm << " " << check.scene
                              << ": draw_calls/vertices differ\n";
                }
            }
            report.structure.push_back(check);
        }
    }

    const ArmRun* reference_run = nullptr;
    for (const ArmRun& run : runs) {
        if (run.opened.arm == Arm::Lavapipe) {
            reference_run = &run;
            break;
        }
    }

    std::vector<RgbaImage> reference_images;
    bool reference_ready = false;
    if (reference_run == nullptr) {
        std::cout << "gpu_backend_parity: comparison skipped (no lavapipe reference backend)\n";
    } else {
        const std::vector<std::filesystem::path> paths = scene_png_paths(reference_dir);
        reference_ready = true;
        for (const std::filesystem::path& path : paths) {
            const std::optional<std::string> bytes = read_bytes(path);
            if (!bytes.has_value()) {
                std::cout << "gpu_backend_parity: comparison skipped (reference image missing: "
                          << display_path(path) << ")\n";
                reference_ready = false;
                break;
            }
            const std::expected<RgbaImage, ImageError> decoded =
                tessera::harness::decode_png(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()));
            if (!decoded.has_value()) {
                std::cerr << "gpu_backend_parity: reference image is not a PNG: "
                          << display_path(path) << "\n";
                return 2;
            }
            reference_images.push_back(*decoded);
        }
    }

    if (reference_ready) {
        const std::span<const Scene> scenes = parity_scenes();
        const std::vector<std::filesystem::path> reference_paths = scene_png_paths(reference_dir);
        for (std::size_t scene_index = 0; scene_index < scenes.size(); ++scene_index) {
            SceneReport scene_report;
            scene_report.scene = std::string(scenes[scene_index].name);
            for (const ArmRun& run : runs) {
                if (!run.opened.available() || run.opened.structure_only ||
                    run.opened.arm == Arm::Lavapipe) {
                    continue;
                }
                const RenderedScene& rendered = run.scenes[scene_index];
                const RgbaImage actual{rendered.width, rendered.height, rendered.rgba};
                const RgbaImage& reference = reference_images[scene_index];

                PairReport pair_report;
                pair_report.comparison =
                    compare_pair(scene_report.scene, run.opened.name, "lavapipe",
                                 {actual.width, actual.height}, actual.rgba, reference.rgba);
                pair_report.artifacts.actual =
                    display_path(actual_root / run.opened.name / (scene_report.scene + ".png"));
                pair_report.artifacts.reference = display_path(reference_paths[scene_index]);

                if (!pair_report.comparison.passed) {
                    report.passed = false;
                    const std::expected<std::string, ImageError> png = diff_png(
                        actual, reference, pair_report.comparison.tolerance.edge_tolerance);
                    if (!png.has_value()) {
                        std::cerr << "gpu_backend_parity: cannot build the diff image for "
                                  << pair_report.comparison.pair << "\n";
                        return 2;
                    }
                    const std::filesystem::path diff_path =
                        diff_root /
                        (pair_report.comparison.pair + "_" + scene_report.scene + ".diff.png");
                    std::filesystem::create_directories(diff_root, error);
                    if (error || !write_bytes(diff_path, *png)) {
                        std::cerr << "gpu_backend_parity: cannot write '" << display_path(diff_path)
                                  << "'\n";
                        return 2;
                    }
                    pair_report.artifacts.diff = display_path(diff_path);
                    std::cout << "gpu_backend_parity: FAIL " << pair_report.comparison.pair << " "
                              << scene_report.scene << ": " << pair_report.comparison.reason << "\n"
                              << "gpu_backend_parity: diff written: " << pair_report.artifacts.diff
                              << "\n";
                }
                scene_report.pairs.push_back(std::move(pair_report));
            }
            report.scenes.push_back(std::move(scene_report));
        }
        report.compared = true;
    }

    if (!deterministic) {
        report.passed = false;
    }
    if (render_failed) {
        report.passed = false;
    }

    const std::filesystem::path report_path = options.out_dir / "parity-report.json";
    if (!write_bytes(report_path, parity_report_json(report))) {
        std::cerr << "gpu_backend_parity: cannot write '" << display_path(report_path) << "'\n";
        return 2;
    }
    std::cout << "gpu_backend_parity: report written: " << display_path(report_path) << "\n";
    std::cout << "gpu_backend_parity: verdict " << (report.passed ? "pass" : "fail")
              << " (compared=" << (report.compared ? "yes" : "no")
              << ", scenes=" << parity_scenes().size() << ")\n";
    if (runs.empty()) {
        std::cout << "gpu_backend_parity: skipped (no RHI backend arm could be opened)\n";
        return 0;
    }
    if (report.passed && !report.compared) {
        std::cout << "gpu_backend_parity: note: no reference-to-candidate pixel comparison was "
                     "possible on this host (see the skipped lines above)\n";
    }
    return report.passed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }
    if (options.help) {
        print_usage(std::cout);
        return 0;
    }
    return options.write_reference ? run_reference(options) : run_compare(options);
}
