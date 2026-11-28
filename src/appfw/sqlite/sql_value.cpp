// SPDX-License-Identifier: MIT

#include "tessera/appfw/sql.hpp"

namespace tessera::appfw::sql {

std::string_view sql_error_code_name(SqlErrorCode code) noexcept {
    switch (code) {
    case SqlErrorCode::None:
        return "none";
    case SqlErrorCode::Cancelled:
        return "cancelled";
    case SqlErrorCode::NotOpen:
        return "not open";
    case SqlErrorCode::AlreadyOpen:
        return "already open";
    case SqlErrorCode::InvalidArgument:
        return "invalid argument";
    case SqlErrorCode::BindCount:
        return "bind count";
    case SqlErrorCode::TypeMismatch:
        return "type mismatch";
    case SqlErrorCode::TooLarge:
        return "too large";
    case SqlErrorCode::Constraint:
        return "constraint";
    case SqlErrorCode::Busy:
        return "busy";
    case SqlErrorCode::Migration:
        return "migration";
    case SqlErrorCode::Sqlite:
        return "sqlite";
    case SqlErrorCode::Unsupported:
        return "unsupported";
    }
    return "unknown";
}

namespace {

SqlError mismatch(std::string_view wanted) {
    return SqlError{SqlErrorCode::TypeMismatch, "read", 0, "value is not " + std::string(wanted)};
}

} // namespace

std::expected<std::int64_t, SqlError> Value::as_int64() const {
    if (const auto* v = std::get_if<std::int64_t>(&data_)) {
        return *v;
    }
    return std::unexpected(mismatch("an integer"));
}

std::expected<double, SqlError> Value::as_double() const {
    if (const auto* v = std::get_if<double>(&data_)) {
        return *v;
    }
    return std::unexpected(mismatch("a double"));
}

std::expected<std::string_view, SqlError> Value::as_string() const {
    if (const auto* v = std::get_if<std::string>(&data_)) {
        return std::string_view(*v);
    }
    return std::unexpected(mismatch("text"));
}

std::expected<std::span<const std::uint8_t>, SqlError> Value::as_blob() const {
    if (const auto* v = std::get_if<Blob>(&data_)) {
        return std::span<const std::uint8_t>(*v);
    }
    return std::unexpected(mismatch("a blob"));
}

} // namespace tessera::appfw::sql
