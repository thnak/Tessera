// SPDX-License-Identifier: MIT
//
// Harness frame capture: the software-device capability rule (no device needed), and an offscreen
// render of a small scene compared against the committed baseline in
// `tests/golden/baseline/harness/` (golden mode, so a lavapipe CI run compares against a reviewed
// image) or against itself (gpu-smoke, whose baseline would be hardware-dependent). Skips the
// render when the mode's device is unavailable (no Vulkan; golden without lavapipe) instead of
// failing. Regenerate the committed baseline on a software device only, through the same ADR-009
// rule the CLI uses:
//
//     harness_frame_capture_tests --update-goldens

#include "tessera/harness/frame_capture.hpp"
#include "tessera/harness/frame_check.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

#ifndef TESSERA_HARNESS_GOLDEN_DIR
#error "TESSERA_HARNESS_GOLDEN_DIR must point at tests/golden/baseline/harness"
#endif

namespace {

using namespace tessera::harness;

int failures = 0;

// `--update-goldens` rewrites the committed golden baseline from the software path only (ADR-009: a
// gpu-smoke run never produces or updates a baseline). The ctest entry never passes it.
bool update_goldens = false;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_frame_capture: " << message << "\n";
        ++failures;
    }
    return condition;
}

class MemorySink final : public ArtifactSink {
public:
    std::expected<void, ProtocolError> write(std::string_view path,
                                             std::string_view bytes) override {
        files[std::string(path)] = std::string(bytes);
        return {};
    }
    std::map<std::string, std::string> files;
};

constexpr std::string_view kSheet = R"(
View { width: 240px; padding: 16px; border-radius: 12px; background-color: #1e293b; }
Text { font-size: 16px; color: #e2e8f0; }
)";

void build_scene(tessera::ui::Node& root) {
    root.set_element_kind("View");
    auto text = std::make_unique<tessera::ui::Node>(2);
    tessera::ui::Node& label = root.add_child(std::move(text));
    label.set_element_kind("Text");
    tessera::ui::PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = "Harness";
    label.set_paint_data(data);
}

void test_capability_rules() {
    expect(!selects_only_software_icd(""), "no ICD list is not software");
    expect(selects_only_software_icd("/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"),
           "lavapipe manifest is software");
    expect(selects_only_software_icd("C:\\mesa\\lvp_icd.x86_64.json"),
           "a Windows lavapipe manifest is software");
    expect(!selects_only_software_icd("C:\\amd\\amd_icd64.json"), "a hardware ICD is not software");
    expect(!selects_only_software_icd("C:\\mesa\\lvp_icd.x86_64.json;C:\\amd\\amd_icd64.json"),
           "a mixed list could expose hardware and is rejected");

    const auto golden = capture_capability(CompareMode::Golden, "");
    expect(!golden && golden.error().code == CaptureErrorCode::NoSoftwareDevice &&
               golden.error().unavailable(),
           "golden without lavapipe is a typed unavailable error");
    expect(capture_capability(CompareMode::Golden, "lvp_icd.x86_64.json").has_value(),
           "golden with lavapipe is available");
    expect(capture_capability(CompareMode::GpuSmoke, "").has_value(),
           "gpu-smoke accepts any device");
}

// The committed baseline is a reviewed lavapipe image, so the golden run proves the harness capture
// path against it instead of against itself. gpu-smoke may use any device (hardware included), so
// its baseline would not be portable: it keeps the self-comparison that only proves the check path.
void test_render(CompareMode mode) {
    tessera::ui::Node root(1);
    build_scene(root);
    tessera::app::CaptureOptions options;
    options.width = 320;
    options.height = 200;
    options.font_path = TESSERA_TEST_FONT;

    auto first = capture_scene_frame(mode, root, kSheet, options);
    if (!first) {
        if (first.error().unavailable()) {
            std::cout << "harness_frame_capture: skipped " << to_string(mode) << " ("
                      << first.error().message << ")\n";
            return;
        }
        expect(false, "capture failed with a render error");
        return;
    }
    expect(first->width == 320 && first->height == 200, "frame has the requested extent");
    bool painted = false;
    for (std::size_t i = 0; i < first->rgba.size(); i += 4) {
        if (first->rgba[i] != 11 || first->rgba[i + 1] != 18 || first->rgba[i + 2] != 32) {
            painted = true;
            break;
        }
    }
    expect(painted, "the scene painted over the clear color");

    const bool golden = mode == CompareMode::Golden;
    const bool update = golden && update_goldens;

    std::filesystem::path baseline;
    std::filesystem::path temporary;
    std::error_code ec;
    if (golden) {
        baseline = std::filesystem::path(TESSERA_HARNESS_GOLDEN_DIR) / "fixture_golden.png";
        if (!update) {
            expect(std::filesystem::exists(baseline), "the committed golden baseline exists");
        }
    } else {
        temporary = std::filesystem::temp_directory_path() / "tessera_harness_capture_tests";
        std::filesystem::create_directories(temporary, ec);
        baseline = temporary / (std::string(to_string(mode)) + ".png");
        const auto png = encode_png(*first);
        if (png) {
            std::ofstream out(baseline, std::ios::binary | std::ios::trunc);
            out.write(png->data(), static_cast<std::streamsize>(png->size()));
        }
    }

    const auto second = capture_scene_frame(mode, root, kSheet, options);
    expect(second.has_value(), "second capture succeeds");
    if (second) {
        MemorySink sink;
        FrameCheckRequest request;
        request.mode = mode;
        request.baseline_path = baseline.string();
        request.update_baseline = update;
        if (golden) {
            // The fixture paints text, and glyph antialiasing rounds differently across lavapipe/
            // LLVM revisions, so the committed baseline uses the same small budget the batched2d
            // text goldens use (docs/specs/golden-tests-v1.md section 1) instead of bit-exact.
            request.policy = ComparePolicy{1, 0.001};
        }
        const auto checked = check_frame(*second, request, sink);
        expect(sink.files.count("frame.png") == 1, "the frame artifact was written");
        if (!checked) {
            std::cerr << "harness_frame_capture: frame check failed: " << checked.error().message
                      << "\n";
            ++failures;
        } else if (update) {
            expect(checked->baseline_updated, "the golden run rewrote the committed baseline");
        } else {
            if (!expect(checked->ok(),
                        "the capture matches its baseline under the mode's policy")) {
                std::cerr << "harness_frame_capture: outcome "
                          << to_string(checked->compare.outcome) << "\n";
            }
        }
    }
    if (!temporary.empty()) {
        std::filesystem::remove_all(temporary, ec);
    }
}

} // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--update-goldens") {
            update_goldens = true;
            continue;
        }
        std::cerr << "harness_frame_capture: unknown argument " << argv[index] << "\n";
        return 2;
    }
    test_capability_rules();
    test_render(CompareMode::GpuSmoke);
    test_render(CompareMode::Golden);
    if (failures != 0) {
        std::cerr << "harness_frame_capture: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "harness_frame_capture: ok\n";
    return 0;
}
