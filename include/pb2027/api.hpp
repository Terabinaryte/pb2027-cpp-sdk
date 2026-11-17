#pragma once

// Optional HTTP module for the pb2027 protocol.
//
// This header is deliberately limited to the standard library plus the core
// value types (types.hpp / error.hpp). The HTTP and JSON implementation is
// confined to src/http.cpp, so linking pb2027::api is required only by users
// that actually fetch metadata, tokens or public boards.

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "pb2027/error.hpp"
#include "pb2027/types.hpp"

namespace pb2027::api {

struct Endpoint {
    std::string host;
    std::uint16_t port = 443;
    bool tls = true;
    bool verify_peer = true;
    std::string ca_file;
    std::string base_path = "/";  // non-root deployments
    std::optional<ProxyConfig> proxy;
    std::optional<std::string> proxy_username, proxy_password;
    std::chrono::milliseconds request_timeout{10000};
};

struct Meta {
    std::uint32_t protocol;
    Rules rules;                  // /api/meta rules + limits merged into Rules
    std::uint16_t width, height;  // from /api/meta's board
    std::string captcha_provider, captcha_site_key, captcha_api_endpoint;
    std::string access_mode, luogu_paste_text;
    std::uint64_t public_version;
    std::uint64_t server_time_ms;
};

struct IssuedToken {
    Uid uid;
    PaintToken token;
    std::optional<std::string> access_key;
};

struct AccessKeyRequest {
    std::string access_key;
};

struct CaptchaRequest {
    std::string captcha;
    std::string paste;
};

// Test-only issuance (issuance.access_mode == "none"): the server takes the
// uid from the request body and issues a token for it.
struct UidRequest {
    Uid uid = 0;
};

using TokenRequest = std::variant<AccessKeyRequest, CaptchaRequest, UidRequest>;

struct BoardResponse {
    enum class Kind : std::uint8_t { NotModified, Snapshot, Public };
    Kind kind;
    std::uint64_t version;
    std::optional<Snapshot> snapshot;   // kind == Snapshot
    std::vector<PublicBatch> batches;   // kind == Public; may contain 0 batches
};

std::expected<Meta, Error> fetch_meta(const Endpoint&);

std::expected<IssuedToken, Error> acquire_token(
    const Endpoint&, TokenRequest);

std::expected<BoardResponse, Error> fetch_board(
    const Endpoint&, std::optional<std::uint64_t> since);

}  // namespace pb2027::api
