#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <zstd.h>

#include "pb2027/pb2027.hpp"

using namespace pb2027;
using byte = std::byte;

namespace {

struct Buf {
    std::vector<byte> v;

    Buf& u8(std::uint8_t x) {
        v.push_back(static_cast<byte>(x));
        return *this;
    }
    Buf& u16(std::uint16_t x) {
        u8(static_cast<std::uint8_t>(x & 0xFF));
        u8(static_cast<std::uint8_t>((x >> 8) & 0xFF));
        return *this;
    }
    Buf& u32(std::uint32_t x) {
        u16(static_cast<std::uint16_t>(x & 0xFFFF));
        u16(static_cast<std::uint16_t>((x >> 16) & 0xFFFF));
        return *this;
    }
    Buf& u64(std::uint64_t x) {
        u32(static_cast<std::uint32_t>(x & 0xFFFFFFFF));
        u32(static_cast<std::uint32_t>((x >> 32) & 0xFFFFFFFF));
        return *this;
    }
    Buf& zeros(std::size_t n) {
        v.insert(v.end(), n, byte{0});
        return *this;
    }
};

std::vector<byte> decompress(const std::vector<byte>& frame) {
    std::vector<byte> out(1u << 20);
    const std::size_t n =
        ZSTD_decompress(out.data(), out.size(), frame.data(), frame.size());
    if (ZSTD_isError(n)) {
        throw std::runtime_error(ZSTD_getErrorName(n));
    }
    out.resize(n);
    return out;
}

std::vector<byte> make_rules_payload() {
    Buf b;
    b.u8(0x84).u8(1).u16(32);
    b.u16(1000).u16(600).u16(2).u16(0);
    b.u32(128);
    b.u64(1'000'000).u64(100'000);
    b.u64(60'000).u64(1'000);
    b.u64(0).u64(0);
    b.u32(8'000).u32(65'536).u32(128).u32(128).u32(1u << 20).u32(1u << 20);
    b.u64(1'234'567'890);
    REQUIRE(b.v.size() == 96);
    return b.v;
}

codec::DecodeLimits limits_for(const Rules& r) {
    codec::DecodeLimits l;
    l.width = r.width;
    l.height = r.height;
    l.max_slots = r.max_slots;
    return l;
}

}  // namespace

TEST_CASE("PaintToken UUID roundtrip") {
    const std::string text = "00010203-0405-0607-0809-0a0b0c0d0e0f";
    auto token = PaintToken::from_uuid(text);
    REQUIRE(token.has_value());
    CHECK(token->to_uuid() == text);
    CHECK(token->bytes[0] == byte{0x00});
    CHECK(token->bytes[1] == byte{0x01});
    CHECK(token->bytes[15] == byte{0x0f});

    CHECK(PaintToken::from_uuid("000102030405060708090a0b0c0d0e0f").has_value());
    CHECK_FALSE(PaintToken::from_uuid("").has_value());
    CHECK_FALSE(PaintToken::from_uuid("zz").has_value());
    CHECK_FALSE(PaintToken::from_uuid("00010203-0405-0607-0809-0a0b0c0d0e0").has_value());
}

TEST_CASE("Batch wire order is binds before paints") {
    Batch b;
    auto token = *PaintToken::from_uuid("00010203-0405-0607-0809-0a0b0c0d0e0f");
    b.paint(2, 10, Color::rgba(1, 2, 3));
    b.bind(1, token);
    b.unbind(3);

    auto records = b.wire_records();
    REQUIRE(records.size() == 3);
    CHECK(records[0].kind == RecordKind::Bind);
    CHECK(records[0].slot == 1);
    CHECK(records[1].kind == RecordKind::Unbind);
    CHECK(records[1].slot == 3);
    CHECK(records[2].kind == RecordKind::Paint);
    CHECK(records[2].slot == 2);
}

TEST_CASE("BATCH encoding layout") {
    Rules r;
    r.width = 1000;
    r.height = 600;
    r.max_slots = 8000;
    r.max_paints_per_message = 65536;

    auto token = *PaintToken::from_uuid("00010203-0405-0607-0809-0a0b0c0d0e0f");
    Batch b;
    b.bind(0, token);
    b.paint(1, 600, Color::rgba(0x11, 0x22, 0x33));

    auto frame = codec::encode_batch(b, r);
    REQUIRE(frame.has_value());
    auto p = decompress(*frame);

    // 16 header + pad4(2) + 16 token + pad4(2) + 4 idx + 4 color
    REQUIRE(p.size() == 48);
    CHECK(std::to_integer<int>(p[0]) == 0x01);
    CHECK(std::to_integer<int>(p[1]) == 1);
    CHECK(std::to_integer<int>(p[2]) == 0);
    CHECK(std::to_integer<int>(p[3]) == 0);

    auto u16 = [&](std::size_t o) {
        return std::to_integer<std::uint16_t>(p[o]) |
               (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[o + 1])) << 8);
    };
    auto u32 = [&](std::size_t o) {
        return static_cast<std::uint32_t>(u16(o)) |
               (static_cast<std::uint32_t>(u16(o + 2)) << 16);
    };

    CHECK(u32(4) == 1);  // nb
    CHECK(u32(8) == 1);  // np
    CHECK(u16(16) == 0); // bind.slot
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(std::to_integer<int>(p[20 + i]) == static_cast<int>(i));
    }
    CHECK(u16(36) == 1);      // paint.slot
    CHECK(u32(40) == 600);    // paint.idx
    CHECK(u32(44) == Color::rgba(0x11, 0x22, 0x33).v);
}

TEST_CASE("BATCH rejects bad alpha and slot") {
    Rules r;
    r.width = 1000;
    r.height = 600;
    r.max_slots = 8;

    Batch b;
    b.paint(9, 0, Color::rgba(0, 0, 0));
    CHECK_FALSE(codec::encode_batch(b, r).has_value());

    Batch b2;
    b2.paint(0, 0, Color{0x00112233u});
    CHECK_FALSE(codec::encode_batch(b2, r).has_value());
}

TEST_CASE("RULES decode") {
    auto payload = make_rules_payload();
    auto msg = codec::decode_payload(payload, {});
    REQUIRE(msg.has_value());
    auto* r = std::get_if<Rules>(&*msg);
    REQUIRE(r != nullptr);
    CHECK(r->width == 1000);
    CHECK(r->height == 600);
    CHECK(r->public_tps == 32);
    CHECK(r->sonar_radius_px == 2);
    CHECK(r->tps == 128);
    CHECK(r->cooldown_base_us == 1'000'000);
    CHECK(r->cooldown_per_px_us == 100'000);
    CHECK(r->max_slots == 8'000);
    CHECK(r->server_time_us == 1'234'567'890);

    payload[0] = byte{0x81};
    CHECK_FALSE(codec::decode_payload(payload, {}).has_value());
}

TEST_CASE("TICK decode with brush, result, sonar and lost slot") {
    const std::uint32_t R = 2;
    const std::uint32_t A = 2 * R * R + 2 * R + 1;
    const std::uint32_t nr = 1, nb = 1, nk = 1, ns = 1;

    Buf b;
    b.u8(0x81).u8(1).u16(R);
    b.u32(nr).u64(42).u32(nb).u32(nk).u32(ns).u32(0);
    // brush.t, brush.idx, brush.slot
    b.u64(1'000).u32(500).u16(0).zeros(2);
    // result.status (pad4), result.value
    b.u8(0).zeros(3).u32(0);
    // sonar.idx
    b.u32(500);
    // sonar.color: 13 cells, all valid except 0 sentinels
    for (std::uint32_t i = 0; i < A; ++i) {
        b.u32(i == 0 ? 0u : 0xFF000000u | i);
    }
    // lost.slot
    b.u16(1).zeros(2);

    auto msg = codec::decode_payload(b.v, {});
    REQUIRE(msg.has_value());
    auto* t = std::get_if<Tick>(&*msg);
    REQUIRE(t != nullptr);
    CHECK(t->number == 42);
    REQUIRE(t->results.size() == 1);
    CHECK(t->results[0].code == ResultCode::Ok);
    REQUIRE(t->brushes.size() == 1);
    CHECK(t->brushes[0].slot == 0);
    REQUIRE(t->brushes[0].brush.idx.has_value());
    CHECK(*t->brushes[0].brush.idx == 500);
    CHECK(t->brushes[0].brush.t_us == 1'000);
    REQUIRE(t->sonars.size() == 1);
    CHECK(t->sonars[0].colors.size() == A);
    CHECK(t->sonars[0].colors[0].v == 0);
    CHECK(t->sonars[0].colors[1].v == (0xFF000000u | 1u));
    REQUIRE(t->lost.size() == 1);
    CHECK(t->lost[0] == 1);
}

TEST_CASE("TICK rejects non-increasing brush slots") {
    Buf b;
    b.u8(0x81).u8(1).u16(0);
    b.u32(0).u64(1).u32(2).u32(0).u32(0).u32(0);
    // two brush entries, slots 5 and 5
    b.u64(1).u64(1).u32(0).u32(0).u16(5).u16(5).zeros(0);
    // lengths: 32 + 8*2 + 4*2 + pad4(2*2)=4 => 32+16+8+4 = 60
    auto msg = codec::decode_payload(b.v, {});
    CHECK_FALSE(msg.has_value());
}

TEST_CASE("SNAPSHOT codec 0 and 1") {
    Buf b0;
    b0.u8(0x83).u8(1).u8(0).u8(0).u16(2).u16(2).u64(7);
    for (std::uint32_t i = 0; i < 4; ++i) {
        b0.u32(0xFF000000u | i);
    }
    auto m0 = codec::decode_payload(b0.v, {});
    REQUIRE(m0.has_value());
    auto* s0 = std::get_if<Snapshot>(&*m0);
    REQUIRE(s0 != nullptr);
    CHECK(s0->width == 2);
    CHECK(s0->height == 2);
    CHECK(s0->version == 7);
    CHECK(s0->codec == SnapshotCodec::Full);
    CHECK(s0->pixels.size() == 4);
    CHECK(s0->l1.empty());

    Buf b1;
    b1.u8(0x83).u8(1).u8(1).u8(0).u16(2).u16(2).u64(8);
    for (std::uint32_t i = 0; i < 4; ++i) b1.u32(0xFF000000u | i);
    for (std::uint32_t i = 0; i < 4; ++i) b1.u32(i == 0 ? 0u : 0xFF000000u | (i + 10));
    for (std::uint32_t i = 0; i < 4; ++i) b1.u32(0u);
    auto m1 = codec::decode_payload(b1.v, {});
    REQUIRE(m1.has_value());
    auto* s1 = std::get_if<Snapshot>(&*m1);
    REQUIRE(s1 != nullptr);
    CHECK(s1->codec == SnapshotCodec::FullWithHistory);
    CHECK(s1->l1.size() == 4);
    CHECK(s1->l1[0].v == 0);
    CHECK(s1->l1[1].v == (0xFF000000u | 11u));
}

TEST_CASE("SNAPSHOT rejects bad alpha") {
    Buf b;
    b.u8(0x83).u8(1).u8(0).u8(0).u16(1).u16(1).u64(1);
    b.u32(0x00112233u);
    CHECK_FALSE(codec::decode_payload(b.v, {}).has_value());
}

TEST_CASE("PUBLIC decode order 0") {
    Buf b;
    b.u8(0x82).u8(1).u16(32);
    b.u32(2).u64(10).u64(20).u32(1).u32(0).u32(0);
    b.u32(5).u32(2);          // seg.tick, seg.n
    b.u8(3).u8(2).zeros(2);   // dsmall + pad
    b.u32(0xFF010203u).u32(2); // colors

    auto msg = codec::decode_payload(b.v, {});
    REQUIRE(msg.has_value());
    auto* p = std::get_if<PublicBatch>(&*msg);
    REQUIRE(p != nullptr);
    CHECK(p->sample_rate == 32);
    CHECK(p->from == 10);
    CHECK(p->to == 20);
    CHECK(p->order == 0);
    REQUIRE(p->segments.size() == 1);
    CHECK(p->segments[0].tick == 5);
    REQUIRE(p->segments[0].entries.size() == 2);
    CHECK(p->segments[0].entries[0].key == 3);
    CHECK(p->segments[0].entries[1].key == 5);
    CHECK(p->segments[0].entries[0].color == 0xFF010203u);
    CHECK(p->segments[0].entries[1].color == 2u);
}

TEST_CASE("PUBLIC decode big delta") {
    Buf b;
    b.u8(0x82).u8(1).u16(32);
    b.u32(1).u64(10).u64(20).u32(1).u32(1).u32(1);
    b.u32(5).u32(1);
    b.u8(255).zeros(3);
    b.u32(1000);
    b.u32(0xFF000000u);

    auto msg = codec::decode_payload(b.v, {});
    REQUIRE(msg.has_value());
    auto* p = std::get_if<PublicBatch>(&*msg);
    REQUIRE(p != nullptr);
    REQUIRE(p->segments.size() == 1);
    REQUIRE(p->segments[0].entries.size() == 1);
    CHECK(p->segments[0].entries[0].key == 1000);
}

TEST_CASE("PUBLIC rejects big-delta count mismatch") {
    Buf b;
    b.u8(0x82).u8(1).u16(32);
    b.u32(1).u64(10).u64(20).u32(1).u32(1).u32(0);
    b.u32(5).u32(1);
    b.u8(255).zeros(3);
    b.u32(0xFF000000u);  // no dbig column actually: e=1 but total computed expects 4 bytes
    CHECK_FALSE(codec::decode_payload(b.v, {}).has_value());
}

TEST_CASE("decode_frame rejects concatenated frames") {
    Rules r;
    r.width = 10;
    r.height = 10;
    r.max_slots = 8;
    r.max_paints_per_message = 100;

    Batch b;
    b.paint(0, 0, Color::rgba(1, 2, 3));
    auto f = codec::encode_batch(b, r);
    REQUIRE(f.has_value());

    std::vector<byte> two = *f;
    two.insert(two.end(), f->begin(), f->end());
    auto msg = codec::decode_frame(two, {});
    CHECK_FALSE(msg.has_value());
}

TEST_CASE("decode_frame rejects plain text") {
    std::vector<byte> text = {byte{'h'}, byte{'i'}};
    CHECK_FALSE(codec::decode_frame(text, {}).has_value());
}

TEST_CASE("decode_payload rejects unknown type as UnknownMessage") {
    Buf b;
    b.u8(0x99).u8(1);
    auto msg = codec::decode_payload(b.v, {});
    REQUIRE(msg.has_value());
    auto* u = std::get_if<UnknownMessage>(&*msg);
    REQUIRE(u != nullptr);
    CHECK(u->type == 0x99);
}
