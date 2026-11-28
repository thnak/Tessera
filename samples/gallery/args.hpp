// SPDX-License-Identifier: MIT
//
// The gallery's command line, parsed by `parse_args` in main.cpp.
#pragma once

#include <string>
#include <vector>

#ifndef TESSERA_GALLERY_FONT
#define TESSERA_GALLERY_FONT "packages/tessera-fonts/ibm-plex/fonts/IBMPlexSans-Regular.ttf"
#endif

namespace gallery {

struct Args {
    std::string capture;
    std::string golden;
    std::string artifact_dir = "gallery-artifacts";
    bool update_goldens = false;
    // Every `--font`: the first is the primary face, each further one another weight of the family
    // (picked per run by `font-weight` from the weight the file declares, #973). Empty means the
    // default face; `parse_args` fills it in.
    std::vector<std::string> fonts;
    std::string theme = "instrument-light";
    std::string density = "comfortable";
    std::string state = "none";
    std::string click;
    std::string emit_module_css;
    int advance_ms = 0;
    int text_scale = 100;
    bool rtl = false;
    bool hidden = false;
    int frames = 0;
    std::string page = "buttons";
    std::string device = "none";
    int width = 0;
    int height = 0;
};

} // namespace gallery
