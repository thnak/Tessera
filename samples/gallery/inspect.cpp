// SPDX-License-Identifier: MIT

#include "inspect.hpp"
#include "stb_image_write.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace gallery_inspect {
namespace {

bool write_png(const std::string& path, const tessera::app::CapturedFrame& frame) {
    return stbi_write_png(path.c_str(), static_cast<int>(frame.width),
                          static_cast<int>(frame.height), 4, frame.rgba.data(),
                          static_cast<int>(frame.width) * 4) != 0;
}

constexpr const char* kHtmlHead = R"html(<!doctype html>
<meta charset="utf-8">
<title>Tessera gallery captures</title>
<style>
  body { background:#14161a; color:#e6e8eb; font:13px/1.5 system-ui, sans-serif; margin:24px; }
  h1 { font-size:16px; font-weight:600; }
  p.note { color:#9aa3ad; max-width:72ch; }
  .grid { display:grid; grid-template-columns:repeat(auto-fill, minmax(280px, 1fr)); gap:16px; }
  figure { margin:0; background:#1c1f24; border:1px solid #2a2f36; border-radius:8px; padding:8px; }
  img { width:100%; height:auto; display:block; border-radius:4px; }
  figcaption { margin-top:6px; color:#9aa3ad; font-family:ui-monospace, monospace; font-size:11px; }
</style>
<h1>Tessera gallery &mdash; captured on this machine</h1>
<p class="note">These are <strong>inspection</strong> images from whatever Vulkan device is
installed. They are not the committed lavapipe goldens and are not byte-comparable with them;
the CI goldens under <code>tests/golden/gallery/</code> remain the authority.</p>
<div class="grid">
)html";

constexpr const char* kHtmlTail = R"html(</div>
)html";

} // namespace

std::vector<Case> case_matrix() {
    std::vector<Case> cases;

    // The three extra cases the CTest matrix adds on top of theme x state. `dark_none` is not
    // repeated here: the theme loop below already produces it.
    {
        Case compact;
        compact.name = "compact";
        compact.density = "compact";
        cases.push_back(compact);
    }
    {
        Case scale;
        scale.name = "scale_150";
        scale.text_scale = 150;
        cases.push_back(scale);
    }
    {
        Case rtl;
        rtl.name = "rtl";
        rtl.rtl = true;
        cases.push_back(rtl);
    }

    for (const std::string theme : {"light", "dark", "high-contrast"}) {
        for (const std::string state : {"none", "hover", "active", "focus"}) {
            Case item;
            item.name = theme + "_" + state;
            item.theme = "instrument-" + theme;
            item.state = state;
            cases.push_back(std::move(item));
        }
    }

    for (const std::string device : {"notch", "gesture", "keyboard", "fold"}) {
        for (const std::string theme : {"light", "dark"}) {
            Case item;
            item.name = "safe_area_" + device + "_" + theme;
            item.page = "safe-area";
            item.device = device;
            item.theme = "instrument-" + theme;
            cases.push_back(std::move(item));
        }
    }
    return cases;
}

bool write_index(const std::string& out_dir, const std::vector<std::string>& written) {
    std::ofstream out(out_dir + "/index.html", std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << kHtmlHead;
    for (const std::string& name : written) {
        out << "    <figure><img src=\"" << name << ".png\" alt=\"" << name << "\"><figcaption>"
            << name << "</figcaption></figure>\n";
    }
    out << kHtmlTail;
    return out.good();
}

int run(const std::string& out_dir, const RenderCase& render) {
    std::error_code error;
    std::filesystem::create_directories(out_dir, error);
    if (error) {
        std::cerr << "gallery: cannot create " << out_dir << ": " << error.message() << "\n";
        return 4;
    }

    const std::vector<Case> cases = case_matrix();
    std::vector<std::string> written;
    int index = 0;
    for (const Case& item : cases) {
        ++index;
        std::optional<tessera::app::CapturedFrame> frame = render(item);
        if (!frame) {
            std::cout << "gallery: skipped (Vulkan device unavailable)\n";
            return 0;
        }
        const std::string path = out_dir + "/" + item.name + ".png";
        if (!write_png(path, *frame)) {
            std::cerr << "gallery: failed to write " << path << "\n";
            return 4;
        }
        written.push_back(item.name);
        std::cout << "gallery: [" << index << "/" << cases.size() << "] " << item.name << " ("
                  << frame->width << "x" << frame->height << ")\n";
    }
    if (!write_index(out_dir, written)) {
        std::cerr << "gallery: failed to write " << out_dir << "/index.html\n";
        return 4;
    }
    std::cout << "gallery: wrote " << written.size() << " captures and " << out_dir
              << "/index.html\n";
    std::cout << "gallery: these are inspection images, not the lavapipe goldens\n";
    return 0;
}

} // namespace gallery_inspect