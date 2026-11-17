#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include <zstd.h>

#include "pb2027/api.hpp"
#include "pb2027/codec.hpp"

using namespace pb2027;
using pb2027::api::Endpoint;
using byte = std::byte;

// src/api.cpp / src/http.cpp internal entry points. These are intentionally
// declared here instead of in a public header: the module's allowed file set
// has no internal header, and tests need to exercise the parsers without a
// server (PLAN §9.1).
namespace pb2027::api::detail {

std::expected<Meta, Error> parse_meta_json(std::string_view body);

std::expected<IssuedToken, Error> parse_issued_token_json(std::string_view body);

std::string make_token_request_json(const TokenRequest& request);

std::expected<BoardResponse, Error> parse_board_payload(
    std::span<const std::byte> payload, std::optional<std::uint64_t> since,
    std::optional<std::uint64_t> header_version);

std::expected<std::vector<std::byte>, Error> zstd_decompress(
    std::span<const std::byte> frame);

}  // namespace pb2027::api::detail

namespace {

struct Buf {
    std::vector<byte> v;

    Buf& u8(std::uint8_t x) {
        v.push_back(static_cast<byte>(x));
        return *this;
    }
    Buf& u16(std::uint16_t x) {
        u8(static_cast<std::uint8_t>(x & 0xFFu));
        u8(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
        return *this;
    }
    Buf& u32(std::uint32_t x) {
        u16(static_cast<std::uint16_t>(x & 0xFFFFu));
        u16(static_cast<std::uint16_t>((x >> 16) & 0xFFFFu));
        return *this;
    }
    Buf& u64(std::uint64_t x) {
        u32(static_cast<std::uint32_t>(x & 0xFFFFFFFFu));
        u32(static_cast<std::uint32_t>((x >> 32) & 0xFFFFFFFFu));
        return *this;
    }
    Buf& pad4() {
        while ((v.size() & 3u) != 0) {
            v.push_back(byte{0});
        }
        return *this;
    }
};

// One PUBLIC message with 1 segment, 2 entries, u8 deltas only.
std::vector<byte> make_public(std::uint16_t sample_rate, std::uint64_t from,
                              std::uint64_t to, std::uint32_t order,
                              std::uint32_t key0, std::uint32_t color0,
                              std::uint32_t key1, std::uint32_t color1) {
    REQUIRE(key1 > key0);
    REQUIRE(key1 - key0 < 255);
    Buf b;
    const std::uint32_t entries = 2;
    const std::uint32_t segments = 1;
    const std::uint32_t big_deltas = 0;
    b.u8(0x82).u8(1).u16(sample_rate).u32(entries).u64(from).u64(to);
    b.u32(segments).u32(big_deltas).u32(order);
    b.u32(0);  // seg.tick
    b.u32(entries);
    b.u8(static_cast<std::uint8_t>(key0));
    b.u8(static_cast<std::uint8_t>(key1 - key0));
    b.pad4();
    b.u32(color0).u32(color1);
    return b.v;
}

// SNAPSHOT codec 1 (the form /api/board returns), 2x1 pixels.
std::vector<byte> make_snapshot(std::uint16_t width, std::uint16_t height,
                                std::uint64_t version, Color c0, Color c1) {
    REQUIRE(static_cast<std::uint32_t>(width) * height == 2);
    Buf b;
    b.u8(0x83).u8(1).u8(1).u8(0).u16(width).u16(height).u64(version);
    b.u32(c0.v).u32(c1.v);
    b.u32(0).u32(0);  // l1
    b.u32(0).u32(0);  // l2
    return b.v;
}

std::vector<byte> compress_zstd(std::span<const byte> payload,
                                bool with_content_size = true) {
    if (with_content_size) {
        std::vector<byte> out(ZSTD_compressBound(payload.size()));
        const std::size_t written =
            ZSTD_compress(out.data(), out.size(), payload.data(),
                          payload.size(), 3);
        REQUIRE_FALSE(ZSTD_isError(written));
        out.resize(written);
        return out;
    }

    ZSTD_CCtx* context = ZSTD_createCCtx();
    REQUIRE(context != nullptr);
    REQUIRE_FALSE(
        ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 0)));
    std::vector<byte> out(ZSTD_compressBound(payload.size()));
    const std::size_t written =
        ZSTD_compress2(context, out.data(), out.size(), payload.data(),
                       payload.size());
    ZSTD_freeCCtx(context);
    REQUIRE_FALSE(ZSTD_isError(written));
    out.resize(written);
    return out;
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string percent_decode(std::string_view text) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex(text[i + 1]);
            const int lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::optional<std::uint16_t> parse_port(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value == 0 || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

// HTTP proxies are the only kind cpp-httplib can tunnel through here; the
// environment also carries socks:// in ALL_PROXY, which we deliberately skip.
bool apply_env_proxy(Endpoint& endpoint) {
    const char* const names[] = {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY",
                                 "http_proxy"};
    for (const char* name : names) {
        const char* raw = std::getenv(name);
        if (raw == nullptr || *raw == '\0') {
            continue;
        }
        std::string_view value(raw);
        std::string scheme = "http";
        if (const auto scheme_end = value.find("://");
            scheme_end != std::string_view::npos) {
            scheme = lower_ascii(std::string(value.substr(0, scheme_end)));
            value.remove_prefix(scheme_end + 3);
        }
        if (scheme != "http") {
            continue;
        }
        if (const auto slash = value.find('/'); slash != std::string_view::npos) {
            value = value.substr(0, slash);
        }

        std::string username;
        std::string password;
        if (const auto at = value.rfind('@'); at != std::string_view::npos) {
            const std::string_view userinfo = value.substr(0, at);
            value = value.substr(at + 1);
            const auto colon = userinfo.find(':');
            if (colon == std::string_view::npos) {
                username = percent_decode(userinfo);
            } else {
                username = percent_decode(userinfo.substr(0, colon));
                password = percent_decode(userinfo.substr(colon + 1));
            }
        }

        std::string host;
        std::string_view port_text;
        if (!value.empty() && value.front() == '[') {
            const auto close = value.find(']');
            if (close == std::string_view::npos) {
                continue;
            }
            host = std::string(value.substr(1, close - 1));
            if (close + 1 < value.size() && value[close + 1] == ':') {
                port_text = value.substr(close + 2);
            }
        } else {
            const auto colon = value.rfind(':');
            if (colon == std::string_view::npos) {
                host = std::string(value);
            } else {
                host = std::string(value.substr(0, colon));
                port_text = value.substr(colon + 1);
            }
        }

        auto port = parse_port(port_text);
        if (host.empty() || !port) {
            continue;
        }

        endpoint.proxy = ProxyConfig{host, *port};
        if (!username.empty() || !password.empty()) {
            endpoint.proxy_username = username;
            endpoint.proxy_password = password;
        }
        return true;
    }
    return false;
}

}  // namespace

TEST_CASE("parse_meta_json merges rules and limits into Rules") {
    const std::string body = R"json({
        "protocol": 1,
        "board": {"width": 1000, "height": 600},
        "rules": {
            "tps": 128,
            "cooldown_base_us": 1000000,
            "cooldown_per_px_us": 100000,
            "sonar_radius_px": 2,
            "public_delay_ms": 60000,
            "public_period_ms": 1000,
            "event_start_unix_ms": 0,
            "event_end_unix_ms": 0
        },
        "limits": {
            "max_slots": 8000,
            "max_paints_per_message": 65536,
            "msg_rate": 128,
            "msg_burst": 128,
            "max_message_bytes": 1048576,
            "max_payload_bytes": 1048576
        },
        "issuance": {
            "captcha_provider": "none",
            "captcha_site_key": "site-key",
            "captcha_api_endpoint": "https://pb.ika.zip/cap/abc/",
            "access_mode": "none",
            "luogu_paste_text": "ikapb"
        },
        "public_version": 17654316,
        "server_time_ms": 1794907189234
    })json";

    auto meta = api::detail::parse_meta_json(body);
    REQUIRE(meta.has_value());

    CHECK(meta->protocol == 1);
    CHECK(meta->width == 1000);
    CHECK(meta->height == 600);
    CHECK(meta->rules.width == 1000);
    CHECK(meta->rules.height == 600);

    CHECK(meta->rules.tps == 128);
    CHECK(meta->rules.cooldown_base_us == 1000000);
    CHECK(meta->rules.cooldown_per_px_us == 100000);
    CHECK(meta->rules.sonar_radius_px == 2);
    CHECK(meta->rules.public_delay_ms == 60000);
    CHECK(meta->rules.public_period_ms == 1000);

    CHECK(meta->rules.max_slots == 8000);
    CHECK(meta->rules.max_paints_per_message == 65536);
    CHECK(meta->rules.msg_rate == 128);
    CHECK(meta->rules.msg_burst == 128);
    CHECK(meta->rules.max_message_bytes == 1048576);
    CHECK(meta->rules.max_payload_bytes == 1048576);

    CHECK(meta->captcha_provider == "none");
    CHECK(meta->captcha_site_key == "site-key");
    CHECK(meta->captcha_api_endpoint == "https://pb.ika.zip/cap/abc/");
    CHECK(meta->access_mode == "none");
    CHECK(meta->luogu_paste_text == "ikapb");

    CHECK(meta->public_version == 17654316);
    CHECK(meta->server_time_ms == 1794907189234);
    CHECK(meta->rules.server_time_us == meta->server_time_ms * 1000);
}

TEST_CASE("parse_meta_json rejects malformed responses") {
    auto malformed = api::detail::parse_meta_json("{not json");
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().kind == ErrorKind::Protocol);

    auto not_object = api::detail::parse_meta_json("[]");
    REQUIRE_FALSE(not_object.has_value());
    CHECK(not_object.error().kind == ErrorKind::Protocol);

    auto missing_limits = api::detail::parse_meta_json(R"json({
        "protocol": 1,
        "board": {"width": 10, "height": 10},
        "rules": {
            "tps": 128,
            "cooldown_base_us": 1,
            "cooldown_per_px_us": 1,
            "sonar_radius_px": 2,
            "public_delay_ms": 1,
            "public_period_ms": 1,
            "event_start_unix_ms": 0,
            "event_end_unix_ms": 0
        },
        "issuance": {},
        "public_version": 1,
        "server_time_ms": 1
    })json");
    REQUIRE_FALSE(missing_limits.has_value());
    CHECK(missing_limits.error().kind == ErrorKind::Protocol);

    auto zero_board = api::detail::parse_meta_json(R"json({
        "protocol": 1,
        "board": {"width": 0, "height": 600},
        "rules": {
            "tps": 128,
            "cooldown_base_us": 1,
            "cooldown_per_px_us": 1,
            "sonar_radius_px": 2,
            "public_delay_ms": 1,
            "public_period_ms": 1,
            "event_start_unix_ms": 0,
            "event_end_unix_ms": 0
        },
        "limits": {
            "max_slots": 1,
            "max_paints_per_message": 1,
            "msg_rate": 1,
            "msg_burst": 1,
            "max_message_bytes": 1,
            "max_payload_bytes": 1
        },
        "issuance": {},
        "public_version": 1,
        "server_time_ms": 1
    })json");
    REQUIRE_FALSE(zero_board.has_value());
    CHECK(zero_board.error().kind == ErrorKind::Protocol);
}

TEST_CASE("parse_issued_token_json accepts the documented response") {
    auto issued = api::detail::parse_issued_token_json(R"json({
        "uid": 123,
        "paint_token": "0f8c2a5e-1d3b-4c5a-8e7f-0123456789ab",
        "access_key": "abc"
    })json");
    REQUIRE(issued.has_value());
    CHECK(issued->uid == 123);
    CHECK(issued->token.to_uuid() == "0f8c2a5e-1d3b-4c5a-8e7f-0123456789ab");
    REQUIRE(issued->access_key.has_value());
    CHECK(*issued->access_key == "abc");

    auto no_access_key = api::detail::parse_issued_token_json(
        R"json({"uid": 7, "paint_token": "0f8c2a5e1d3b4c5a8e7f0123456789ab"})json");
    REQUIRE(no_access_key.has_value());
    CHECK(no_access_key->uid == 7);
    CHECK_FALSE(no_access_key->access_key.has_value());

    auto bad_token = api::detail::parse_issued_token_json(
        R"json({"uid": 7, "paint_token": "not-a-uuid"})json");
    REQUIRE_FALSE(bad_token.has_value());
    CHECK(bad_token.error().kind == ErrorKind::Protocol);

    auto missing_uid = api::detail::parse_issued_token_json(
        R"json({"paint_token": "0f8c2a5e1d3b4c5a8e7f0123456789ab"})json");
    REQUIRE_FALSE(missing_uid.has_value());
    CHECK(missing_uid.error().kind == ErrorKind::Protocol);
}

TEST_CASE("make_token_request_json matches the protocol bodies") {
    CHECK(api::detail::make_token_request_json(
              api::AccessKeyRequest{"abc"}) == R"({"access_key":"abc"})");
    CHECK(api::detail::make_token_request_json(
              api::CaptchaRequest{"cap", "paste"}) ==
          R"({"captcha":"cap","paste":"paste"})");
    CHECK(api::detail::make_token_request_json(api::UidRequest{123}) ==
          R"({"uid":123})");
}

TEST_CASE("parse_board_payload splits concatenated PUBLIC messages") {
    const Color c0 = Color::rgba(1, 2, 3);
    const Color c1 = Color::rgba(4, 5, 6);
    const Color d0 = Color::rgba(7, 8, 9);
    const Color d1 = Color::rgba(10, 11, 12);

    auto first = make_public(128, 10, 11, 0, 3, c0.v, 4, c1.v);
    auto second = make_public(128, 11, 12, 1, 5, d0.v, 6, d1.v);
    std::vector<byte> both = first;
    both.insert(both.end(), second.begin(), second.end());

    auto parsed =
        api::detail::parse_board_payload(both, 10, std::nullopt);
    REQUIRE(parsed.has_value());
    CHECK(parsed->kind == api::BoardResponse::Kind::Public);
    CHECK(parsed->version == 12);  // fallback: last batch's `to`
    CHECK_FALSE(parsed->snapshot.has_value());
    REQUIRE(parsed->batches.size() == 2);

    const auto& a = parsed->batches[0];
    CHECK(a.sample_rate == 128);
    CHECK(a.from == 10);
    CHECK(a.to == 11);
    CHECK(a.order == 0);
    REQUIRE(a.segments.size() == 1);
    REQUIRE(a.segments[0].entries.size() == 2);
    CHECK(a.segments[0].entries[0].key == 3);
    CHECK(a.segments[0].entries[0].color == c0.v);
    CHECK(a.segments[0].entries[1].key == 4);
    CHECK(a.segments[0].entries[1].color == c1.v);

    const auto& b = parsed->batches[1];
    CHECK(b.from == 11);
    CHECK(b.to == 12);
    CHECK(b.order == 1);
    REQUIRE(b.segments.size() == 1);
    REQUIRE(b.segments[0].entries.size() == 2);
    CHECK(b.segments[0].entries[0].key == 5);
    CHECK(b.segments[0].entries[1].key == 6);

    auto with_header = api::detail::parse_board_payload(both, 10, 99);
    REQUIRE(with_header.has_value());
    CHECK(with_header->version == 99);
}

TEST_CASE("parse_board_payload decodes SNAPSHOT") {
    const Color c0 = Color::rgba(1, 2, 3);
    const Color c1 = Color::rgba(4, 5, 6);
    auto payload = make_snapshot(2, 1, 42, c0, c1);

    auto parsed =
        api::detail::parse_board_payload(payload, std::nullopt, std::nullopt);
    REQUIRE(parsed.has_value());
    CHECK(parsed->kind == api::BoardResponse::Kind::Snapshot);
    CHECK(parsed->version == 42);
    REQUIRE(parsed->snapshot.has_value());
    CHECK(parsed->snapshot->width == 2);
    CHECK(parsed->snapshot->height == 1);
    CHECK(parsed->snapshot->version == 42);
    CHECK(parsed->snapshot->codec == SnapshotCodec::FullWithHistory);
    REQUIRE(parsed->snapshot->pixels.size() == 2);
    CHECK(parsed->snapshot->pixels[0].v == c0.v);
    CHECK(parsed->snapshot->pixels[1].v == c1.v);
    CHECK(parsed->batches.empty());

    auto truncated = payload;
    truncated.pop_back();
    auto bad = api::detail::parse_board_payload(truncated, std::nullopt,
                                                std::nullopt);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().kind == ErrorKind::Protocol);
}

TEST_CASE("parse_board_payload accepts a full-size codec-1 SNAPSHOT") {
    // 1000x600 codec 1 is 16 + 12*W*H = 7,200,016 bytes, far above the
    // 1 MiB default codec limit for /ws frames. /api/board must accept it.
    constexpr std::uint16_t width = 1000;
    constexpr std::uint16_t height = 600;
    const std::size_t count = static_cast<std::size_t>(width) * height;

    Buf b;
    b.u8(0x83).u8(1).u8(1).u8(0).u16(width).u16(height).u64(123);
    b.v.reserve(16 + 12 * count);
    const Color pixel = Color::rgba(9, 8, 7);
    for (std::size_t i = 0; i < count; ++i) {
        b.u32(pixel.v);
    }
    for (std::size_t i = 0; i < count; ++i) {
        b.u32(0);  // l1
    }
    for (std::size_t i = 0; i < count; ++i) {
        b.u32(0);  // l2
    }
    REQUIRE(b.v.size() == 16 + 12 * count);
    REQUIRE(b.v.size() > (1u << 20));

    auto parsed =
        api::detail::parse_board_payload(b.v, std::nullopt, std::nullopt);
    REQUIRE(parsed.has_value());
    CHECK(parsed->kind == api::BoardResponse::Kind::Snapshot);
    CHECK(parsed->version == 123);
    REQUIRE(parsed->snapshot.has_value());
    CHECK(parsed->snapshot->width == width);
    CHECK(parsed->snapshot->height == height);
    CHECK(parsed->snapshot->pixels.size() == count);
    CHECK(parsed->snapshot->pixels.front().v == pixel.v);
    CHECK(parsed->snapshot->l1.size() == count);
    CHECK(parsed->snapshot->l2.size() == count);
}

TEST_CASE("zstd_decompress handles board frames") {
    auto payload =
        make_public(128, 1, 2, 0, 3, Color::rgba(1, 2, 3).v, 4,
                    Color::rgba(4, 5, 6).v);

    // Both the common one-shot frame (content size present) and a frame with
    // no content size (streaming fallback) must decompress identically.
    for (const bool with_content_size : {true, false}) {
        auto frame = compress_zstd(payload, with_content_size);
        auto raw = api::detail::zstd_decompress(frame);
        REQUIRE(raw.has_value());
        CHECK(std::equal(raw->begin(), raw->end(), payload.begin(),
                         payload.end()));
    }

    std::vector<byte> garbage = {byte{'n'}, byte{'o'}, byte{'t'}, byte{'z'}};
    auto bad = api::detail::zstd_decompress(garbage);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().kind == ErrorKind::Protocol);

    auto empty = api::detail::zstd_decompress({});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().kind == ErrorKind::Protocol);
}

TEST_CASE("fetch_meta rejects an empty endpoint without a request") {
    api::Endpoint endpoint;
    endpoint.host.clear();
    auto meta = api::fetch_meta(endpoint);
    REQUIRE_FALSE(meta.has_value());
    CHECK(meta.error().kind == ErrorKind::InvalidArgument);
}

// One live request only (PLAN §9.1): GET /api/meta via the environment proxy.
TEST_CASE("fetch_meta against the live server") {
    api::Endpoint endpoint;
    endpoint.host = "pb.ika.zip";
    endpoint.port = 443;
    endpoint.tls = true;
    endpoint.verify_peer = true;
    endpoint.request_timeout = std::chrono::milliseconds(20000);
    const bool proxied = apply_env_proxy(endpoint);

    INFO("proxy=" << (proxied ? std::string("env") : std::string("none")));
    auto meta = api::fetch_meta(endpoint);
    if (!meta) {
        FAIL("fetch_meta failed: " << to_string(meta.error()));
        return;
    }

    MESSAGE("live meta: protocol=" << meta->protocol << " board="
            << meta->width << "x" << meta->height << " max_slots="
            << meta->rules.max_slots << " cooldown_base_us="
            << meta->rules.cooldown_base_us << " cooldown_per_px_us="
            << meta->rules.cooldown_per_px_us << " public_version="
            << meta->public_version);

    CHECK(meta->protocol == 1);
    CHECK(meta->width > 0);
    CHECK(meta->height > 0);
    CHECK(meta->rules.width == meta->width);
    CHECK(meta->rules.height == meta->height);
    CHECK(meta->rules.max_slots > 0);
    CHECK(meta->rules.max_slots <= 65536);
    CHECK(meta->rules.cooldown_base_us > 0);
    CHECK(meta->rules.cooldown_per_px_us > 0);
    CHECK(meta->rules.tps > 0);
    CHECK(meta->rules.max_paints_per_message > 0);
    CHECK(meta->server_time_ms > 0);
    CHECK(meta->rules.server_time_us == meta->server_time_ms * 1000);
}
