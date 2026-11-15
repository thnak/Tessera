// SPDX-License-Identifier: MIT

#include "mcp_protocol.hpp"

namespace tessera::mcp {

bool protocol_version_supported(std::string_view version) noexcept {
    for (const std::string_view supported : kSupportedProtocolVersions) {
        if (supported == version) {
            return true;
        }
    }
    return false;
}

} // namespace tessera::mcp
