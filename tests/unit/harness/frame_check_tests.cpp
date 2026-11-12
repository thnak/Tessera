// SPDX-License-Identifier: MIT
//
// Image compare + frame check rules for `--mode golden` / `--mode gpu-smoke`: PNG round trip,
// exact vs tolerant policy, diff image, baseline update gating. CPU only, no device.

#include "tessera/harness/frame_check.hpp"
#include "tessera/harness/image_compare.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace tessera::harness;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_check: " << message << "\n";
        ++failures;
    }
    return condition;
}

RgbaImage solid(std::uint32_t width, std::uint32_t height, std::uint8_t r, std::uint8_t g,
                std::uint8_t b) {
    RgbaImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = r;
        image.rgba[i + 1] = g;
        image.rgba[i + 2] = b;
        image.rgba[i + 3] = 255;
    }
    return image;
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

std::span<const std::uint8_t> as_bytes(const std::string& text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

void write_png_file(const std::filesystem::path& path, const RgbaImage& image) {
    const auto png = encode_png(image);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(png->data(), static_cast<std::streamsize>(png->size()));
}

void test_png_round_trip() {
    RgbaImage image = solid(3, 2, 10, 20, 30);
    image.rgba[0] = 200;
    const auto png = encode_png(image);
    expect(png.has_value(), "encode succeeds");
    const auto back = decode_png(as_bytes(*png));
    expect(back.has_value() && back->width == 3 && back->height == 2 && back->rgba == image.rgba,
           "PNG round trip is lossless");
    expect(!decode_png(as_bytes(std::string("not a png"))).has_value(), "garbage is rejected");
    expect(!decode_png({}).has_value(), "empty input is rejected");
    RgbaImage empty;
    expect(!encode_png(empty).has_value(), "empty image does not encode");
}

void test_policy_and_compare() {
    expect(default_policy(CompareMode::Golden).per_channel_tolerance == 0 &&
               default_policy(CompareMode::Golden).max_mismatch_ratio == 0.0,
           "golden policy is exact");
    expect(default_policy(CompareMode::GpuSmoke).per_channel_tolerance > 0,
           "gpu-smoke policy is tolerant");

    const RgbaImage base = solid(4, 4, 100, 100, 100);
    RgbaImage near = base;
    near.rgba[0] = 103;
    const auto golden =
        compare_images(CompareMode::Golden, near, &base, default_policy(CompareMode::Golden));
    expect(golden.outcome == CompareOutcome::Mismatch && golden.diff.mismatched == 1 &&
               golden.diff.max_channel_delta == 3,
           "golden fails on a single 3-level channel delta");
    const auto smoke =
        compare_images(CompareMode::GpuSmoke, near, &base, default_policy(CompareMode::GpuSmoke));
    expect(smoke.passed(), "gpu-smoke tolerates a small channel delta");

    RgbaImage far = base;
    far.rgba[0] = 200;
    expect(compare_images(CompareMode::GpuSmoke, far, &base, ComparePolicy{8, 0.0}).outcome ==
               CompareOutcome::Mismatch,
           "a large delta fails even with tolerance");
    expect(compare_images(CompareMode::GpuSmoke, far, &base, ComparePolicy{8, 0.1}).passed(),
           "a pixel budget absorbs one outlier out of 16");

    const RgbaImage wide = solid(5, 4, 100, 100, 100);
    expect(compare_images(CompareMode::Golden, wide, &base, {}).outcome ==
               CompareOutcome::SizeMismatch,
           "different sizes are a size mismatch");
    expect(compare_images(CompareMode::Golden, base, nullptr, {}).outcome ==
               CompareOutcome::BaselineMissing,
           "no baseline is reported as missing");

    const std::string json = compare_result_json(golden, "dir\\a\"b.png");
    expect(json.find("\"outcome\":\"mismatch\"") != std::string::npos &&
               json.find("\"mismatched\":1") != std::string::npos &&
               json.find("dir\\\\a\\\"b.png") != std::string::npos,
           "report JSON carries outcome, counts, and an escaped baseline");

    const auto diff = make_diff_image(near, base, 0);
    expect(diff.has_value() && diff->rgba[0] == 255 && diff->rgba[1] == 0 && diff->rgba[4] == 25 &&
               diff->rgba[3] == 255,
           "diff image marks the mismatch red and dims the rest");
    expect(!make_diff_image(wide, base, 0).has_value(), "diff image rejects a size mismatch");
}

void test_check_frame() {
    const auto dir = std::filesystem::temp_directory_path() / "tessera_frame_check_tests";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const std::string baseline_path = (dir / "base.png").string();
    const RgbaImage base = solid(4, 4, 100, 100, 100);
    write_png_file(dir / "base.png", base);

    {
        MemorySink sink;
        FrameCheckRequest request;
        request.baseline_path = baseline_path;
        request.artifact_prefix = "run/";
        const auto result = check_frame(base, request, sink);
        expect(result && result->ok() && sink.files.count("run/frame.png") == 1 &&
                   sink.files.count("run/frame.compare.json") == 1 &&
                   sink.files.count("run/frame.diff.png") == 0,
               "a matching golden writes frame + report and no diff");
    }
    {
        MemorySink sink;
        FrameCheckRequest request;
        request.baseline_path = baseline_path;
        RgbaImage changed = base;
        changed.rgba[0] = 101;
        const auto result = check_frame(changed, request, sink);
        expect(result && !result->ok() && sink.files.count("frame.diff.png") == 1 &&
                   result->diff_artifact == "frame.diff.png",
               "an off-by-one golden fails and writes the diff image");
        const auto on_disk = std::ifstream(baseline_path, std::ios::binary);
        expect(on_disk.good(), "baseline still present");
    }
    {
        MemorySink sink;
        FrameCheckRequest request;
        request.mode = CompareMode::GpuSmoke;
        request.baseline_path = baseline_path;
        RgbaImage changed = base;
        changed.rgba[0] = 104;
        const auto result = check_frame(changed, request, sink);
        expect(result && result->ok() && !result->baseline_updated,
               "gpu-smoke passes within tolerance and never updates");
    }
    {
        MemorySink sink;
        FrameCheckRequest request;
        request.mode = CompareMode::GpuSmoke;
        request.baseline_path = baseline_path;
        request.update_baseline = true;
        const auto result = check_frame(solid(4, 4, 1, 2, 3), request, sink);
        expect(!result && result.error().code == FrameCheckErrorCode::UpdateNotAllowed &&
                   sink.files.empty(),
               "gpu-smoke refuses an update before writing anything");
        std::ifstream in(baseline_path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        const auto still = decode_png(as_bytes(bytes));
        expect(still && still->rgba == base.rgba, "baseline bytes are untouched by gpu-smoke");
    }
    {
        MemorySink sink;
        FrameCheckRequest request;
        request.update_baseline = true; // no baseline path
        expect(!check_frame(base, request, sink), "update without a baseline path is rejected");
    }
    {
        MemorySink sink;
        FrameCheckRequest request;
        request.baseline_path = (dir / "missing.png").string();
        const auto result = check_frame(base, request, sink);
        expect(result && !result->ok() &&
                   result->compare.outcome == CompareOutcome::BaselineMissing,
               "a missing baseline fails without update");
        request.update_baseline = true;
        const auto updated = check_frame(base, request, sink);
        expect(updated && updated->ok() && updated->baseline_updated &&
                   std::filesystem::exists(dir / "missing.png"),
               "golden with update writes the new baseline");
        const auto again = check_frame(base, request, sink);
        expect(again && again->compare.passed() && !again->baseline_updated,
               "an update run over a matching baseline rewrites nothing");
    }
    {
        std::ofstream(dir / "bad.png", std::ios::binary) << "junk";
        MemorySink sink;
        FrameCheckRequest request;
        request.baseline_path = (dir / "bad.png").string();
        request.update_baseline = true;
        const auto result = check_frame(base, request, sink);
        expect(!result && result.error().code == FrameCheckErrorCode::BaselineInvalid,
               "a corrupt baseline is an error, not a silent overwrite");
    }
    std::filesystem::remove_all(dir, ec);
}

} // namespace

int main() {
    test_png_round_trip();
    test_policy_and_compare();
    test_check_frame();
    if (failures != 0) {
        std::cerr << "frame_check: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "frame_check: ok\n";
    return 0;
}
