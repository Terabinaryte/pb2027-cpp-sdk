// Public api::* orchestration on top of the detail HTTP layer.
//
// The HTTP/JSON implementation lives in src/http.cpp; this file maps responses
// to protocol values, splits concatenated PUBLIC messages and maps statuses to
// pb2027::Error.

#include "pb2027/api.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "pb2027/codec.hpp"
#include "pb2027/error.hpp"
#include "pb2027/types.hpp"

namespace pb2027::api {

namespace detail {

// Defined in src/http.cpp. Declared here (rather than in an internal header)
// because the allowed file set for this module has no internal header; the
// definitions must be kept in sync.
struct HttpResult {
    int status = 0;
    std::string body;
    std::string board_version;  // X-Board-Version, empty when absent
    std::string etag;           // ETag, empty when absent
    std::string retry_after;    // Retry-After, empty when absent
};

std::expected<HttpResult, Error> http_get(
    const Endpoint&, std::string_view,
    const std::vector<std::pair<std::string, std::string>>&);

std::expected<HttpResult, Error> http_post_json(const Endpoint&, std::string_view,
                                                std::string_view);

std::expected<Meta, Error> parse_meta_json(std::string_view);

std::expected<IssuedToken, Error> parse_issued_token_json(std::string_view);

std::string make_token_request_json(
    const TokenRequest&);

std::expected<std::vector<std::byte>, Error> zstd_decompress(
    std::span<const std::byte>);

// Exposed inside detail so tests can exercise the /api/board payload parser
// without a network round trip.
std::expected<BoardResponse, Error> parse_board_payload(
    std::span<const std::byte>, std::optional<std::uint64_t> since,
    std::optional<std::uint64_t> header_version);

}  // namespace detail

namespace {

using byte = std::byte;

Error make_error(ErrorKind kind, std::string message) {
    return Error{kind, std::move(message), std::nullopt, std::nullopt};
}

Error protocol_error(std::string message) {
    return make_error(ErrorKind::Protocol, std::move(message));
}

Error prefix_error(Error error, std::string_view prefix) {
    error.message = std::string(prefix) + ": " + error.message;
    return error;
}

std::string_view trim_ascii(std::string_view s) {
    auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<std::uint64_t> parse_decimal_u64(std::string_view text) {
    text = trim_ascii(text);
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// X-Board-Version is the primary source; ETag is the documented fallback
// ("v<Vpub>", optionally quoted/weak).
std::optional<std::uint64_t> response_version(const detail::HttpResult& response) {
    if (auto version = parse_decimal_u64(response.board_version)) {
        return version;
    }
    std::string_view etag = trim_ascii(response.etag);
    if (etag.size() >= 2 && etag.substr(0, 2) == "W/") {
        etag.remove_prefix(2);
    }
    if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"') {
        etag = etag.substr(1, etag.size() - 2);
    }
    if (!etag.empty() && (etag.front() == 'v' || etag.front() == 'V')) {
        etag.remove_prefix(1);
    }
    return parse_decimal_u64(etag);
}

std::optional<std::chrono::seconds> parse_retry_after(std::string_view text) {
    const auto seconds = parse_decimal_u64(text);
    if (!seconds) {
        return std::nullopt;
    }
    if (*seconds >
        static_cast<std::uint64_t>(std::chrono::seconds::max().count())) {
        return std::chrono::seconds::max();
    }
    return std::chrono::seconds(static_cast<std::int64_t>(*seconds));
}

Error status_error(const detail::HttpResult& response, std::string_view what) {
    std::string message(what);
    message += ": HTTP ";
    message += std::to_string(response.status);
    std::string_view body = trim_ascii(response.body);
    if (!body.empty()) {
        message += ", body=";
        message.append(body.substr(0, 200));
        if (body.size() > 200) {
            message += "...";
        }
    }
    return protocol_error(std::move(message));
}

std::uint32_t read_u32(const byte* p) noexcept {
    const auto u8 = [](const byte* q) {
        return std::to_integer<std::uint32_t>(q[0]);
    };
    return u8(p) | (u8(p + 1) << 8) | (u8(p + 2) << 16) | (u8(p + 3) << 24);
}

std::string hex_byte(std::uint8_t value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.push_back(kHex[value >> 4]);
    out.push_back(kHex[value & 0x0Fu]);
    return out;
}

// /api/board is much larger than a WebSocket message: a codec-1 full snapshot
// for 1000x600 is 16 + 12*W*H = 7,200,016 bytes. The codec defaults (1 MiB)
// are meant for /ws frames, so the board path raises both limits explicitly.
// 64 MiB matches the zstd decompression ceiling in src/http.cpp and still
// bounds allocations on a hostile response.
constexpr std::uint32_t kBoardPayloadLimit = 64u << 20;
constexpr std::uint32_t kBoardMaxEntries = 4u << 20;

codec::DecodeLimits board_decode_limits() {
    codec::DecodeLimits limits{};
    limits.max_message_bytes = kBoardPayloadLimit;
    limits.max_payload_bytes = kBoardPayloadLimit;
    limits.max_entries = kBoardMaxEntries;
    return limits;
}

std::size_t pad4(std::size_t n) noexcept {
    return (n + 3u) & ~std::size_t(3u);
}

std::expected<std::vector<PublicBatch>, Error> split_public_messages(
    std::span<const byte> payload) {
    std::vector<PublicBatch> batches;
    std::size_t offset = 0;
    std::size_t index = 0;

    while (offset < payload.size()) {
        const std::size_t remaining = payload.size() - offset;
        if (remaining < 36) {
            return std::unexpected(protocol_error(
                "board: truncated PUBLIC header at message " +
                std::to_string(index)));
        }

        const byte* header = payload.data() + offset;
        if (header[0] != byte{0x82}) {
            return std::unexpected(protocol_error(
                "board: expected PUBLIC (0x82) at message " +
                std::to_string(index)));
        }

        const std::uint32_t entries = read_u32(header + 4);
        const std::uint32_t segments = read_u32(header + 24);
        const std::uint32_t big_deltas = read_u32(header + 28);

        // 36 + 8m + pad4(n) + 4e + 4n, all in 64-bit to stay overflow-free.
        const std::uint64_t length =
            36ull + 8ull * segments +
            static_cast<std::uint64_t>(pad4(entries)) +
            4ull * big_deltas + 4ull * entries;
        if (length > remaining) {
            return std::unexpected(protocol_error(
                "board: PUBLIC message " + std::to_string(index) +
                " declares " + std::to_string(length) +
                " bytes but only " + std::to_string(remaining) + " remain"));
        }

        codec::DecodeLimits limits = board_decode_limits();
        auto decoded = codec::decode_payload(
            payload.subspan(offset, static_cast<std::size_t>(length)), limits);
        if (!decoded) {
            return std::unexpected(prefix_error(
                std::move(decoded.error()),
                "board PUBLIC[" + std::to_string(index) + "]"));
        }
        auto* batch = std::get_if<PublicBatch>(&*decoded);
        if (batch == nullptr) {
            return std::unexpected(protocol_error(
                "board: PUBLIC message " + std::to_string(index) +
                " decoded to a non-PUBLIC payload"));
        }

        batches.push_back(std::move(*batch));
        offset += static_cast<std::size_t>(length);
        ++index;
    }

    return batches;
}

}  // namespace

namespace detail {

std::expected<BoardResponse, Error> parse_board_payload(
    std::span<const std::byte> payload, std::optional<std::uint64_t> since,
    std::optional<std::uint64_t> header_version) {
    if (payload.empty()) {
        return std::unexpected(protocol_error("board: empty payload"));
    }

    const auto type = std::to_integer<std::uint8_t>(payload.front());
    if (type == 0x83) {
        codec::DecodeLimits limits = board_decode_limits();
        auto decoded = codec::decode_payload(payload, limits);
        if (!decoded) {
            return std::unexpected(
                prefix_error(std::move(decoded.error()), "board SNAPSHOT"));
        }
        auto* snapshot = std::get_if<Snapshot>(&*decoded);
        if (snapshot == nullptr) {
            return std::unexpected(protocol_error(
                "board: SNAPSHOT payload decoded to a non-SNAPSHOT message"));
        }

        BoardResponse response{};
        response.kind = BoardResponse::Kind::Snapshot;
        response.version = header_version.value_or(snapshot->version);
        response.snapshot = std::move(*snapshot);
        return response;
    }

    if (type == 0x82) {
        auto batches = split_public_messages(payload);
        if (!batches) {
            return std::unexpected(std::move(batches.error()));
        }

        BoardResponse response{};
        response.kind = BoardResponse::Kind::Public;
        if (header_version) {
            response.version = *header_version;
        } else if (!batches->empty()) {
            response.version = batches->back().to;
        } else {
            response.version = since.value_or(0);
        }
        response.batches = std::move(*batches);
        return response;
    }

    return std::unexpected(protocol_error(
        "board: unexpected payload type 0x" + hex_byte(type)));
}

}  // namespace detail

std::expected<Meta, Error> fetch_meta(const Endpoint& endpoint) {
    auto response =
        detail::http_get(endpoint, "/api/meta", {{"Accept", "application/json"}});
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    if (response->status != 200) {
        return std::unexpected(status_error(*response, "GET /api/meta"));
    }
    return detail::parse_meta_json(response->body);
}

std::expected<IssuedToken, Error> acquire_token(
    const Endpoint& endpoint,
    TokenRequest request) {
    const std::string body = detail::make_token_request_json(request);
    auto response =
        detail::http_post_json(endpoint, "/api/paint-token", body);
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }

    if (response->status == 429) {
        Error error{ErrorKind::RateLimited,
                    "POST /api/paint-token: HTTP 429", std::nullopt,
                    parse_retry_after(response->retry_after)};
        return std::unexpected(std::move(error));
    }
    if (response->status != 200) {
        return std::unexpected(status_error(*response, "POST /api/paint-token"));
    }
    return detail::parse_issued_token_json(response->body);
}

std::expected<BoardResponse, Error> fetch_board(
    const Endpoint& endpoint, std::optional<std::uint64_t> since) {
    std::string path = "/api/board";
    if (since) {
        path += "?since=";
        path += std::to_string(*since);
    }

    auto response = detail::http_get(
        endpoint, path, {{"Accept", "application/octet-stream"}});
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }

    if (response->status == 304) {
        const auto version = response_version(*response);
        if (!version) {
            return std::unexpected(protocol_error(
                "GET /api/board: 304 without a usable X-Board-Version/ETag"));
        }
        BoardResponse out{};
        out.kind = BoardResponse::Kind::NotModified;
        out.version = *version;
        return out;
    }

    if (response->status == 429) {
        Error error{ErrorKind::RateLimited, "GET /api/board: HTTP 429",
                    std::nullopt,
                    parse_retry_after(response->retry_after)};
        return std::unexpected(std::move(error));
    }

    if (response->status != 200) {
        return std::unexpected(status_error(*response, "GET /api/board"));
    }

    const auto body = std::span<const char>(response->body.data(),
                                            response->body.size());
    auto decompressed = detail::zstd_decompress(std::as_bytes(body));
    if (!decompressed) {
        return std::unexpected(std::move(decompressed.error()));
    }

    return detail::parse_board_payload(*decompressed, since,
                                       response_version(*response));
}

}  // namespace pb2027::api
