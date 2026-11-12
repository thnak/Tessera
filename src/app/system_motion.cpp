// SPDX-License-Identifier: MIT

#include "tessera/app/system_motion.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tessera::app {

bool system_prefers_reduced_motion() noexcept {
#if defined(_WIN32)
    BOOL animations_enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations_enabled, 0) == FALSE) {
        return false; // the query failed: keep animations on
    }
    return animations_enabled == FALSE;
#else
    return false;
#endif
}

} // namespace tessera::app
