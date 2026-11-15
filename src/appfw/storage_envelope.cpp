// SPDX-License-Identifier: MIT

#include "storage_envelope.hpp"
#include "tessera/appfw/service_id.hpp"

#include <cstring>

namespace tessera::appfw::detail {
namespace {

void put_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFu));
    out.push_back(static_cast<char>((value >> 8u) & 0xFFu));
}

void put_u64(std::string& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFu));
    }
}

[[nodiscard]] std::uint16_t read_u16(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes[offset]) |
        (static_cast<unsigned>(static_cast<unsigned char>(bytes[offset + 1])) << 8u));
}

[[nodiscard]] std::uint64_t read_u64(std::string_view bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8u) |
                static_cast<unsigned char>(bytes[offset + static_cast<std::size_t>(index)]);
    }
    return value;
}

template<typename T> [[nodiscard]] std::expected<T, StorageError> corrupted(std::string_view step) {
    return std::unexpected(StorageError{StorageErrorCode::Corrupted, step});
}

} // namespace

std::uint64_t envelope_checksum(std::string_view payload) noexcept {
    return fnv1a(payload);
}

std::expected<std::string, StorageError> encode_envelope(EnvelopeKind kind, std::uint32_t version,
                                                         std::string_view payload,
                                                         std::size_t max_payload) {
    if (payload.size() > max_payload) {
        return std::unexpected(StorageError{StorageErrorCode::TooLarge, "encode-size"});
    }
    std::string bytes;
    bytes.reserve(kEnvelopeHeaderBytes + payload.size());
    bytes.append(kEnvelopeMagic);
    put_u16(bytes, static_cast<std::uint16_t>(version));
    put_u16(bytes, static_cast<std::uint16_t>(kind));
    put_u64(bytes, static_cast<std::uint64_t>(payload.size()));
    put_u64(bytes, envelope_checksum(payload));
    bytes.append(payload);
    return bytes;
}

std::expected<DecodedEnvelope, StorageError> decode_envelope(std::string_view bytes,
                                                             EnvelopeKind expected_kind,
                                                             std::uint32_t max_version,
                                                             std::size_t max_payload) {
    // A file shorter than the header cannot even be a valid empty entry: a truncated write (the
    // failure the temp-file sequence is designed to prevent) lands here.
    if (bytes.size() < kEnvelopeHeaderBytes) {
        return corrupted<DecodedEnvelope>("header-short");
    }
    if (bytes.substr(0, kEnvelopeMagic.size()) != kEnvelopeMagic) {
        return corrupted<DecodedEnvelope>("header-magic");
    }

    const auto version = static_cast<std::uint32_t>(read_u16(bytes, 4));
    const auto kind = static_cast<EnvelopeKind>(read_u16(bytes, 6));
    const std::uint64_t declared = read_u64(bytes, 8);
    const std::uint64_t checksum = read_u64(bytes, 16);

    if (kind != expected_kind) {
        return corrupted<DecodedEnvelope>("header-kind");
    }
    if (version == 0 || version > max_version) {
        // A newer format version is not "corrupt", but it is not readable either: report it as
        // Corrupted so the caller falls back rather than mis-parsing a future layout.
        return corrupted<DecodedEnvelope>("header-version");
    }

    const std::string_view payload = bytes.substr(kEnvelopeHeaderBytes);
    if (declared != payload.size()) {
        return corrupted<DecodedEnvelope>("header-length");
    }
    if (declared > max_payload) {
        return std::unexpected(StorageError{StorageErrorCode::TooLarge, "decode-size"});
    }
    if (envelope_checksum(payload) != checksum) {
        return corrupted<DecodedEnvelope>("checksum");
    }

    DecodedEnvelope decoded;
    decoded.kind = kind;
    decoded.version = version;
    decoded.payload.assign(payload);
    return decoded;
}

} // namespace tessera::appfw::detail
