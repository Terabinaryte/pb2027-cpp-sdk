#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pb2027 {

enum class ErrorKind : std::uint8_t {
    Transport,
    Tls,
    Handshake,
    Protocol,
    Timeout,
    Closed,
    RateLimited,
    Banned,
    InvalidArgument,
    Unsupported,
    Internal,
};

struct Error {
    ErrorKind kind = ErrorKind::Internal;
    std::string message;
    std::optional<std::uint16_t> close_code;
    std::optional<std::chrono::seconds> retry_after;
};

enum class LogLevel : std::uint8_t { Error, Warn, Info, Debug, Trace };

constexpr std::string_view to_string(ErrorKind kind) noexcept {
    switch (kind) {
        case ErrorKind::Transport: return "transport";
        case ErrorKind::Tls: return "tls";
        case ErrorKind::Handshake: return "handshake";
        case ErrorKind::Protocol: return "protocol";
        case ErrorKind::Timeout: return "timeout";
        case ErrorKind::Closed: return "closed";
        case ErrorKind::RateLimited: return "rate_limited";
        case ErrorKind::Banned: return "banned";
        case ErrorKind::InvalidArgument: return "invalid_argument";
        case ErrorKind::Unsupported: return "unsupported";
        case ErrorKind::Internal: return "internal";
    }
    return "unknown";
}

inline std::string to_string(const Error& e) {
    std::string s(to_string(e.kind));
    s += ": ";
    s += e.message;
    return s;
}

}  // namespace pb2027
