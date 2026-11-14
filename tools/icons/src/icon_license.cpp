// SPDX-License-Identifier: MIT

#include "icon_license.hpp"

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::icons {
namespace {

struct Entry {
    std::string_view id;
    LicenseClass license_class;
    std::string_view note;
};

// The ADR-047 allowlist, plus the identifiers that are known to be incompatible so the rejection
// can name the actual reason instead of "unknown".
constexpr std::array<Entry, 22> kEntries{{
    {"mit", LicenseClass::Permissive, {}},
    {"mit-0", LicenseClass::Permissive, {}},
    {"isc", LicenseClass::Permissive, {}},
    {"apache-2.0", LicenseClass::Permissive, {}},
    {"bsd-2-clause", LicenseClass::Permissive, {}},
    {"bsd-3-clause", LicenseClass::Permissive, {}},
    {"bsd-2-clause-patent", LicenseClass::Permissive, {}},
    {"0bsd", LicenseClass::Permissive, {}},
    {"cc0-1.0", LicenseClass::Permissive, {}},
    {"ofl-1.1", LicenseClass::Permissive, {}},
    {"cc-by-4.0", LicenseClass::Attribution, {}},
    {"cc-by-3.0", LicenseClass::Attribution, {}},
    {"cc-by-sa-4.0", LicenseClass::ShareAlike, {}},
    {"cc-by-sa-3.0", LicenseClass::ShareAlike, {}},
    {"cc-by-nc-4.0", LicenseClass::NonCommercial, {}},
    {"cc-by-nc-3.0", LicenseClass::NonCommercial, {}},
    {"bsd-4-clause", LicenseClass::Unlisted,
     "it carries an advertising clause, which the ADR-047 allowlist does not accept"},
    {"mpl-2.0", LicenseClass::Unlisted,
     "weak copyleft is not on the ADR-047 allowlist and needs an owner decision"},
    {"epl-2.0", LicenseClass::Unlisted,
     "weak copyleft is not on the ADR-047 allowlist and needs an owner decision"},
    {"cddl-1.0", LicenseClass::Unlisted,
     "weak copyleft is not on the ADR-047 allowlist and needs an owner decision"},
    {"unlicense", LicenseClass::Unlisted,
     "it is not on the ADR-047 allowlist, which names CC0-1.0 as the public-domain option"},
    {"proprietary", LicenseClass::Unlisted, "a proprietary license cannot be redistributed"},
}};

// Iconify metadata records the license either as an SPDX identifier or as a human title. Titles are
// folded to their SPDX identifier here so the gate reads the same policy either way; an unknown
// title is not guessed at.
constexpr std::array<std::pair<std::string_view, std::string_view>, 19> kTitleAliases{{
    {"apache-license-2.0", "apache-2.0"},
    {"mit-license", "mit"},
    {"isc-license", "isc"},
    {"bsd-2-clause-license", "bsd-2-clause"},
    {"bsd-3-clause-license", "bsd-3-clause"},
    {"cc0", "cc0-1.0"},
    {"cc0-1.0-universal", "cc0-1.0"},
    {"creative-commons-zero-1.0-universal", "cc0-1.0"},
    {"open-font-license-1.1", "ofl-1.1"},
    {"sil-open-font-license-1.1", "ofl-1.1"},
    {"creative-commons-attribution-3.0", "cc-by-3.0"},
    {"creative-commons-attribution-4.0", "cc-by-4.0"},
    {"creative-commons-attribution-share-alike-3.0", "cc-by-sa-3.0"},
    {"creative-commons-attribution-share-alike-4.0", "cc-by-sa-4.0"},
    {"creative-commons-attribution-non-commercial-4.0", "cc-by-nc-4.0"},
    {"gnu-general-public-license-v2.0", "gpl-2.0"},
    {"gnu-general-public-license-v3.0", "gpl-3.0"},
    {"gnu-lesser-general-public-license-v2.1", "lgpl-2.1"},
    {"mozilla-public-license-2.0", "mpl-2.0"},
}};

[[nodiscard]] std::string normalize(std::string_view raw) {
    std::string value;
    value.reserve(raw.size());
    for (const char character : raw) {
        const auto code = static_cast<unsigned char>(character);
        if (std::isspace(code) != 0 || character == '_') {
            if (!value.empty() && value.back() != '-') {
                value.push_back('-');
            }
            continue;
        }
        value.push_back(static_cast<char>(std::tolower(code)));
    }
    while (!value.empty() && value.back() == '-') {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] LicenseInfo unlisted(std::string id, std::string note) {
    LicenseInfo info;
    info.id = std::move(id);
    info.license_class = LicenseClass::Unlisted;
    info.note = std::move(note);
    return info;
}

} // namespace

LicenseInfo classify_license(std::string_view raw) {
    std::string id = normalize(raw);
    if (id.empty()) {
        return unlisted(id, "the set metadata records no license");
    }
    for (const auto& [title, spdx] : kTitleAliases) {
        if (id == title) {
            id = std::string(spdx);
            break;
        }
    }
    for (const Entry& entry : kEntries) {
        if (id == entry.id) {
            return LicenseInfo{id, entry.license_class, std::string(entry.note)};
        }
    }
    // Family prefixes: an unknown variant is still classified by the family its identifier names.
    if (starts_with(id, "gpl-") || id == "gpl" || starts_with(id, "lgpl-") || id == "lgpl" ||
        starts_with(id, "agpl-") || id == "agpl") {
        return LicenseInfo{id, LicenseClass::GplFamily, {}};
    }
    if (starts_with(id, "cc-by-nc")) {
        return LicenseInfo{id, LicenseClass::NonCommercial, {}};
    }
    if (starts_with(id, "cc-by-sa")) {
        return LicenseInfo{id, LicenseClass::ShareAlike, {}};
    }
    if (starts_with(id, "cc-by-")) {
        return LicenseInfo{id, LicenseClass::Attribution, {}};
    }
    if (starts_with(id, "bsd-") || starts_with(id, "cc-") || starts_with(id, "apache-") ||
        starts_with(id, "ofl-") || starts_with(id, "mit")) {
        // A variant of an allowlisted family that the table does not know: do not guess.
        return unlisted(id, "the identifier is a variant the ADR-047 allowlist does not name");
    }
    return unlisted(id, "unknown license identifier; the ADR-047 allowlist does not include it");
}

LicenseGate check_license(std::string_view set, std::string_view raw, bool allow_share_alike) {
    static_cast<void>(set);
    LicenseGate gate;
    gate.license = classify_license(raw);
    switch (gate.license.license_class) {
    case LicenseClass::Permissive:
    case LicenseClass::Attribution:
        gate.accepted = true;
        break;
    case LicenseClass::ShareAlike:
        gate.accepted = allow_share_alike;
        gate.reason = allow_share_alike
                          ? std::string()
                          : "it is share-alike; pass --allow-share-alike to accept it explicitly";
        break;
    case LicenseClass::GplFamily:
        gate.reason =
            "GPL-family licenses are copyleft and cannot be bundled into an MIT application "
            "(ADR-047 item 8)";
        break;
    case LicenseClass::NonCommercial:
        gate.reason = "non-commercial licenses cannot ship in a distributed application";
        break;
    case LicenseClass::Unlisted:
        gate.reason = gate.license.note.empty() ? "the license is not on the ADR-047 allowlist"
                                                : gate.license.note;
        break;
    }
    return gate;
}

bool requires_attribution(LicenseClass license_class) noexcept {
    return license_class == LicenseClass::Attribution;
}

std::string license_rejection_message(std::string_view set, std::string_view raw,
                                      const LicenseGate& gate) {
    std::string message = "cannot import icon set \"";
    message.append(set);
    message.append("\": license \"");
    message.append(raw.empty() ? "none recorded" : raw);
    message.append("\"");
    if (!gate.reason.empty()) {
        message.append(": ");
        message.append(gate.reason);
    }
    return message;
}

} // namespace tessera::icons
