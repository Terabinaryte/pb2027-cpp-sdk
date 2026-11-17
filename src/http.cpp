// HTTP transport + JSON (de)serialization for pb2027::api.
//
// This is the only translation unit that includes cpp-httplib and
// nlohmann/json, keeping both out of the public headers (PLAN §5.8).

#include "pb2027/api.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib/httplib.h>

#include <nlohmann/json.hpp>

#include <zstd.h>

namespace pb2027::api::detail {

struct HttpResult {
    int status = 0;
    std::string body;
    std::string board_version;  // X-Board-Version, empty when absent
    std::string etag;           // ETag, empty when absent
    std::string retry_after;    // Retry-After, empty when absent
};

namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxDecompressedBytes = 64u << 20;  // generous board cap

Error make_error(ErrorKind kind, std::string message) {
    return Error{kind, std::move(message), std::nullopt, std::nullopt};
}

Error protocol_error(std::string message) {
    return make_error(ErrorKind::Protocol, std::move(message));
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// --- URL / client configuration -------------------------------------------

std::string join_path(std::string_view base_path, std::string_view path) {
    std::string base(base_path);
    if (base.empty()) {
        base = "/";
    }
    if (base.front() != '/') {
        base.insert(base.begin(), '/');
    }
    while (base.size() > 1 && base.back() == '/') {
        base.pop_back();
    }

    std::string rel(path);
    if (rel.empty()) {
        rel = "/";
    }
    if (rel.front() != '/') {
        rel.insert(rel.begin(), '/');
    }
    return base == "/" ? rel : base + rel;
}

std::expected<std::string, Error> make_base_url(const Endpoint& ep) {
    std::string host = ep.host;
    bool tls = ep.tls;

    // Accept "https://host" in addition to a bare host; PLAN's Endpoint uses
    // a host, but passing the public URL is a common mistake worth absorbing.
    if (const auto scheme_end = host.find("://"); scheme_end != std::string::npos) {
        const std::string scheme = lower_ascii(host.substr(0, scheme_end));
        host.erase(0, scheme_end + 3);
        if (scheme == "https") {
            tls = true;
        } else if (scheme == "http") {
            tls = false;
        } else {
            return std::unexpected(make_error(
                ErrorKind::InvalidArgument,
                "unsupported endpoint scheme '" + scheme + "'"));
        }
    }
    // A trailing path on the host would make the authority malformed.
    if (const auto slash = host.find('/'); slash != std::string::npos) {
        host.erase(slash);
    }
    if (host.empty()) {
        return std::unexpected(make_error(ErrorKind::InvalidArgument,
                                          "endpoint host is empty"));
    }
    if (ep.port == 0) {
        return std::unexpected(make_error(ErrorKind::InvalidArgument,
                                          "endpoint port is zero"));
    }
    if (host.find(':') != std::string::npos && host.front() != '[') {
        host = "[" + host + "]";  // bare IPv6 literal
    }

    std::string url = tls ? "https://" : "http://";
    url += host;
    url += ':';
    url += std::to_string(ep.port);
    return url;
}

std::expected<void, Error> configure_client(httplib::Client& cli,
                                            const Endpoint& ep) {
    if (!cli.is_valid()) {
        return std::unexpected(make_error(ErrorKind::InvalidArgument,
                                          "invalid HTTP endpoint/client"));
    }

    cli.enable_server_certificate_verification(ep.verify_peer);
    cli.enable_server_hostname_verification(ep.verify_peer);
    if (!ep.ca_file.empty()) {
        cli.set_ca_cert_path(ep.ca_file);
    }

    if (ep.request_timeout.count() > 0) {
        cli.set_connection_timeout(ep.request_timeout);
        cli.set_read_timeout(ep.request_timeout);
        cli.set_write_timeout(ep.request_timeout);
    }

    if (ep.proxy) {
        if (ep.proxy->host.empty() || ep.proxy->port == 0) {
            return std::unexpected(make_error(
                ErrorKind::InvalidArgument,
                "proxy host/port must be non-empty/non-zero"));
        }
        cli.set_proxy(ep.proxy->host, ep.proxy->port);
        if (ep.proxy_username || ep.proxy_password) {
            cli.set_proxy_basic_auth(ep.proxy_username.value_or(std::string{}),
                                     ep.proxy_password.value_or(std::string{}));
        }
    }
    return {};
}

Error transport_error(httplib::Error error, std::string_view what) {
    ErrorKind kind = ErrorKind::Transport;
    switch (error) {
        case httplib::Error::ConnectionTimeout:
        case httplib::Error::Timeout:
            kind = ErrorKind::Timeout;
            break;
        case httplib::Error::SSLConnection:
        case httplib::Error::SSLLoadingCerts:
        case httplib::Error::SSLServerVerification:
        case httplib::Error::SSLServerHostnameVerification:
            kind = ErrorKind::Tls;
            break;
        default:
            break;
    }
    std::string message(what);
    message += ": ";
    message += httplib::to_string(error);
    return make_error(kind, std::move(message));
}

HttpResult to_http_result(const httplib::Response& response) {
    HttpResult out;
    out.status = response.status;
    out.body = response.body;
    out.board_version = response.get_header_value("X-Board-Version");
    out.etag = response.get_header_value("ETag");
    out.retry_after = response.get_header_value("Retry-After");
    return out;
}

// --- JSON helpers ----------------------------------------------------------

[[noreturn]] void field_error(std::string_view where, std::string_view key,
                              std::string_view why) {
    throw std::runtime_error(std::string(where) + '.' + std::string(key) + ": " +
                             std::string(why));
}

const json& require_field(const json& object, const char* key,
                          std::string_view where) {
    const auto it = object.find(key);
    if (it == object.end()) {
        field_error(where, key, "missing");
    }
    return *it;
}

const json& require_object(const json& object, const char* key,
                           std::string_view where) {
    const json& value = require_field(object, key, where);
    if (!value.is_object()) {
        field_error(where, key, "expected object");
    }
    return value;
}

std::uint64_t require_u64(const json& object, const char* key,
                          std::string_view where) {
    const json& value = require_field(object, key, where);
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value >= 0) {
            return static_cast<std::uint64_t>(signed_value);
        }
    }
    field_error(where, key, "expected non-negative integer");
}

std::uint32_t require_u32(const json& object, const char* key,
                          std::string_view where) {
    const std::uint64_t value = require_u64(object, key, where);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        field_error(where, key, "out of uint32 range");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint16_t require_u16(const json& object, const char* key,
                          std::string_view where) {
    const std::uint64_t value = require_u64(object, key, where);
    if (value > std::numeric_limits<std::uint16_t>::max()) {
        field_error(where, key, "out of uint16 range");
    }
    return static_cast<std::uint16_t>(value);
}

std::string require_string(const json& object, const char* key,
                           std::string_view where) {
    const json& value = require_field(object, key, where);
    if (!value.is_string()) {
        field_error(where, key, "expected string");
    }
    return value.get<std::string>();
}

std::string optional_string(const json& object, const char* key,
                            std::string_view where,
                            std::string fallback = {}) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    if (!it->is_string()) {
        field_error(where, key, "expected string");
    }
    return it->get<std::string>();
}

std::expected<json, Error> parse_json_object(std::string_view body,
                                             std::string_view what) {
    try {
        json parsed = json::parse(body.begin(), body.end());
        if (!parsed.is_object()) {
            return std::unexpected(protocol_error(
                std::string(what) + ": response is not a JSON object"));
        }
        return parsed;
    } catch (const std::exception& e) {
        return std::unexpected(protocol_error(std::string(what) +
                                              ": invalid JSON: " + e.what()));
    }
}

// --- zstd ------------------------------------------------------------------

struct ZstdDctxDeleter {
    void operator()(ZSTD_DCtx* context) const noexcept {
        ZSTD_freeDCtx(context);
    }
};

}  // namespace

std::expected<HttpResult, Error> http_get(
    const Endpoint& ep, std::string_view path,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    auto base = make_base_url(ep);
    if (!base) {
        return std::unexpected(std::move(base.error()));
    }

    httplib::Client cli(*base);
    auto configured = configure_client(cli, ep);
    if (!configured) {
        return std::unexpected(std::move(configured.error()));
    }

    httplib::Headers hs;
    for (const auto& [name, value] : headers) {
        hs.emplace(name, value);
    }

    const std::string target = join_path(ep.base_path, path);
    auto response = cli.Get(target, hs);
    if (!response) {
        return std::unexpected(
            transport_error(response.error(), "GET " + target));
    }
    return to_http_result(*response);
}

std::expected<HttpResult, Error> http_post_json(const Endpoint& ep,
                                                std::string_view path,
                                                std::string_view json_body) {
    auto base = make_base_url(ep);
    if (!base) {
        return std::unexpected(std::move(base.error()));
    }

    httplib::Client cli(*base);
    auto configured = configure_client(cli, ep);
    if (!configured) {
        return std::unexpected(std::move(configured.error()));
    }

    const std::string target = join_path(ep.base_path, path);
    auto response = cli.Post(target, std::string(json_body),
                             "application/json");
    if (!response) {
        return std::unexpected(
            transport_error(response.error(), "POST " + target));
    }
    return to_http_result(*response);
}

std::expected<Meta, Error> parse_meta_json(std::string_view body) {
    auto parsed = parse_json_object(body, "meta");
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }

    try {
        const json& root = *parsed;
        Meta meta{};

        meta.protocol = require_u32(root, "protocol", "meta");

        const json& board = require_object(root, "board", "meta");
        meta.width = require_u16(board, "width", "meta.board");
        meta.height = require_u16(board, "height", "meta.board");
        if (meta.width == 0 || meta.height == 0) {
            throw std::runtime_error("meta.board: width and height must be non-zero");
        }

        const json& rules = require_object(root, "rules", "meta");
        meta.rules.width = meta.width;
        meta.rules.height = meta.height;
        meta.rules.tps = require_u32(rules, "tps", "meta.rules");
        meta.rules.cooldown_base_us =
            require_u64(rules, "cooldown_base_us", "meta.rules");
        meta.rules.cooldown_per_px_us =
            require_u64(rules, "cooldown_per_px_us", "meta.rules");
        meta.rules.sonar_radius_px =
            require_u16(rules, "sonar_radius_px", "meta.rules");
        meta.rules.public_delay_ms =
            require_u64(rules, "public_delay_ms", "meta.rules");
        meta.rules.public_period_ms =
            require_u64(rules, "public_period_ms", "meta.rules");
        meta.rules.event_start_unix_ms =
            require_u64(rules, "event_start_unix_ms", "meta.rules");
        meta.rules.event_end_unix_ms =
            require_u64(rules, "event_end_unix_ms", "meta.rules");

        const json& limits = require_object(root, "limits", "meta");
        meta.rules.max_slots = require_u32(limits, "max_slots", "meta.limits");
        meta.rules.max_paints_per_message =
            require_u32(limits, "max_paints_per_message", "meta.limits");
        meta.rules.msg_rate = require_u32(limits, "msg_rate", "meta.limits");
        meta.rules.msg_burst = require_u32(limits, "msg_burst", "meta.limits");
        meta.rules.max_message_bytes =
            require_u32(limits, "max_message_bytes", "meta.limits");
        meta.rules.max_payload_bytes =
            require_u32(limits, "max_payload_bytes", "meta.limits");

        const json& issuance = require_object(root, "issuance", "meta");
        meta.captcha_provider =
            optional_string(issuance, "captcha_provider", "meta.issuance");
        meta.captcha_site_key =
            optional_string(issuance, "captcha_site_key", "meta.issuance");
        meta.captcha_api_endpoint =
            optional_string(issuance, "captcha_api_endpoint", "meta.issuance");
        meta.access_mode =
            optional_string(issuance, "access_mode", "meta.issuance");
        meta.luogu_paste_text =
            optional_string(issuance, "luogu_paste_text", "meta.issuance");

        meta.public_version = require_u64(root, "public_version", "meta");
        meta.server_time_ms = require_u64(root, "server_time_ms", "meta");
        if (meta.server_time_ms >
            std::numeric_limits<std::uint64_t>::max() / 1000u) {
            throw std::runtime_error("meta.server_time_ms: out of range");
        }
        meta.rules.server_time_us = meta.server_time_ms * 1000u;

        return meta;
    } catch (const json::exception& e) {
        return std::unexpected(
            protocol_error(std::string("meta: ") + e.what()));
    } catch (const std::exception& e) {
        return std::unexpected(protocol_error(e.what()));
    }
}

std::expected<IssuedToken, Error> parse_issued_token_json(std::string_view body) {
    auto parsed = parse_json_object(body, "paint-token");
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }

    try {
        const json& root = *parsed;
        IssuedToken issued{};
        issued.uid = require_u32(root, "uid", "paint-token");

        const std::string token_text =
            require_string(root, "paint_token", "paint-token");
        auto token = PaintToken::from_uuid(token_text);
        if (!token) {
            throw std::runtime_error(
                "paint-token.paint_token: malformed UUID");
        }
        issued.token = *token;

        const auto access_key = root.find("access_key");
        if (access_key != root.end() && !access_key->is_null()) {
            if (!access_key->is_string()) {
                field_error("paint-token", "access_key", "expected string");
            }
            issued.access_key = access_key->get<std::string>();
        }

        return issued;
    } catch (const json::exception& e) {
        return std::unexpected(
            protocol_error(std::string("paint-token: ") + e.what()));
    } catch (const std::exception& e) {
        return std::unexpected(protocol_error(e.what()));
    }
}

std::string make_token_request_json(const TokenRequest& request) {
    json body;
    if (const auto* access = std::get_if<AccessKeyRequest>(&request)) {
        body["access_key"] = access->access_key;
    } else if (const auto* uid = std::get_if<UidRequest>(&request)) {
        body["uid"] = uid->uid;
    } else {
        const auto& captcha = std::get<CaptchaRequest>(request);
        body["captcha"] = captcha.captcha;
        body["paste"] = captcha.paste;
    }
    // replace: never throw on caller-supplied non-UTF-8 text.
    return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::expected<std::vector<std::byte>, Error> zstd_decompress(
    std::span<const std::byte> frame) {
    if (frame.empty()) {
        return std::unexpected(protocol_error("HTTP body is an empty zstd frame"));
    }

    const unsigned long long content_size =
        ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (content_size == ZSTD_CONTENTSIZE_ERROR) {
        return std::unexpected(protocol_error("HTTP body is not a valid zstd frame"));
    }

    if (content_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (content_size > kMaxDecompressedBytes) {
            return std::unexpected(protocol_error(
                "zstd frame decompresses to more than the allowed limit"));
        }
        std::vector<std::byte> out(static_cast<std::size_t>(content_size));
        const std::size_t written =
            ZSTD_decompress(out.data(), out.size(), frame.data(), frame.size());
        if (ZSTD_isError(written)) {
            return std::unexpected(protocol_error(
                std::string("zstd decompress failed: ") +
                ZSTD_getErrorName(written)));
        }
        if (written != out.size()) {
            return std::unexpected(protocol_error(
                "zstd decompressed size does not match the frame header"));
        }
        return out;
    }

    // Streaming fallback when the frame does not carry a content size.
    std::unique_ptr<ZSTD_DCtx, ZstdDctxDeleter> context(ZSTD_createDCtx());
    if (!context) {
        return std::unexpected(make_error(ErrorKind::Internal,
                                          "zstd: cannot allocate a DCtx"));
    }

    std::vector<std::byte> out;
    std::array<std::byte, 64u << 10> chunk{};
    ZSTD_inBuffer input{frame.data(), frame.size(), 0};

    std::size_t remaining = 1;
    while (input.pos < input.size || remaining != 0) {
        ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
        const std::size_t before_in = input.pos;
        remaining = ZSTD_decompressStream(context.get(), &output, &input);
        if (ZSTD_isError(remaining)) {
            return std::unexpected(protocol_error(
                std::string("zstd decompress failed: ") +
                ZSTD_getErrorName(remaining)));
        }
        out.insert(out.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(output.pos));
        if (out.size() > kMaxDecompressedBytes) {
            return std::unexpected(protocol_error(
                "zstd frame decompresses to more than the allowed limit"));
        }
        if (remaining == 0) {
            if (input.pos != input.size) {
                return std::unexpected(protocol_error(
                    "HTTP body contains trailing data after the zstd frame"));
            }
            break;
        }
        if (output.pos == 0 && input.pos == before_in) {
            return std::unexpected(
                protocol_error("zstd decompress made no progress"));
        }
    }

    return out;
}

}  // namespace pb2027::api::detail
