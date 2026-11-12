// SPDX-License-Identifier: MIT

#include "tessera/harness/frame_capture.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

namespace tessera::harness {
namespace {

std::string env_value(const char* name) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
        return {};
    }
    std::string value(buffer);
    std::free(buffer); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    return value;
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read once, UI thread
    return value == nullptr ? std::string() : std::string(value);
#endif
}

// The loader separates manifests with ';' on Windows and ':' elsewhere (a drive letter's ':' must
// not split a Windows path).
#if defined(_WIN32)
constexpr const char* kSeparators = ";";
#else
constexpr const char* kSeparators = ";:";
#endif

bool is_lavapipe_manifest(std::string_view entry) noexcept {
    return entry.find("lvp_icd") != std::string_view::npos ||
           entry.find("lavapipe") != std::string_view::npos;
}

} // namespace

bool selects_only_software_icd(std::string_view icd_filenames) noexcept {
    if (icd_filenames.empty()) {
        return false;
    }
    std::size_t begin = 0;
    bool any = false;
    while (begin <= icd_filenames.size()) {
        std::size_t end = icd_filenames.find_first_of(kSeparators, begin);
        if (end == std::string_view::npos) {
            end = icd_filenames.size();
        }
        const std::string_view entry = icd_filenames.substr(begin, end - begin);
        if (!entry.empty()) {
            any = true;
            if (!is_lavapipe_manifest(entry)) {
                return false;
            }
        }
        begin = end + 1;
    }
    return any;
}

std::expected<void, CaptureError> capture_capability(CompareMode mode,
                                                     std::string_view icd_filenames) {
    if (mode == CompareMode::Golden && !selects_only_software_icd(icd_filenames)) {
        return std::unexpected(CaptureError{
            CaptureErrorCode::NoSoftwareDevice,
            "golden frames need the lavapipe software Vulkan driver: set VK_ICD_FILENAMES to "
            "lvp_icd.*.json"});
    }
    return {};
}

std::expected<void, CaptureError> capture_capability(CompareMode mode) {
    if (mode != CompareMode::Golden) {
        return {};
    }
    std::string icd = env_value("VK_ICD_FILENAMES");
    if (icd.empty()) {
        icd = env_value("VK_DRIVER_FILES");
    }
    return capture_capability(mode, icd);
}

std::expected<RgbaImage, CaptureError> capture_scene_frame(CompareMode mode, ui::Node& root,
                                                           std::string_view stylesheet,
                                                           const app::CaptureOptions& options,
                                                           const std::function<void()>& prepare) {
    if (auto capability = capture_capability(mode); !capability) {
        return std::unexpected(std::move(capability.error()));
    }
    try {
        auto captured = app::capture_frame(
            root, ui::style::compile_stylesheet(std::string(stylesheet)), options, prepare);
        if (!captured) {
            return std::unexpected(
                CaptureError{CaptureErrorCode::NoVulkanDevice, "no Vulkan device available"});
        }
        RgbaImage image;
        image.width = captured->width;
        image.height = captured->height;
        image.rgba = std::move(captured->rgba);
        return image;
    } catch (const std::exception& error) {
        return std::unexpected(CaptureError{CaptureErrorCode::RenderFailed, error.what()});
    }
}

} // namespace tessera::harness
