#include "pb2027/codec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace pb2027 {
namespace {

using byte = std::byte;
using codec::DecodeLimits;

constexpr std::size_t pad4(std::size_t n) noexcept {
    return (n + 3u) & ~std::size_t(3u);
}

inline std::uint8_t rd8(const byte* p) noexcept {
    return std::to_integer<std::uint8_t>(p[0]);
}
inline std::uint16_t rd16(const byte* p) noexcept {
    std::uint16_t v = rd8(p);
    v = static_cast<std::uint16_t>(v | (static_cast<std::uint16_t>(rd8(p + 1)) << 8));
    return v;
}
inline std::uint32_t rd32(const byte* p) noexcept {
    std::uint32_t v = rd16(p);
    v |= static_cast<std::uint32_t>(rd16(p + 2)) << 16;
    return v;
}
inline std::uint64_t rd64(const byte* p) noexcept {
    std::uint64_t v = rd32(p);
    v |= static_cast<std::uint64_t>(rd32(p + 4)) << 32;
    return v;
}

inline void put8(std::vector<byte>& out, std::uint8_t v) {
    out.push_back(static_cast<byte>(v));
}
inline void put16(std::vector<byte>& out, std::uint16_t v) {
    put8(out, static_cast<std::uint8_t>(v & 0xFFu));
    put8(out, static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}
inline void put32(std::vector<byte>& out, std::uint32_t v) {
    put16(out, static_cast<std::uint16_t>(v & 0xFFFFu));
    put16(out, static_cast<std::uint16_t>((v >> 16) & 0xFFFFu));
}
inline void put64(std::vector<byte>& out, std::uint64_t v) {
    put32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    put32(out, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
}
inline void pad_to4(std::vector<byte>& out) {
    while ((out.size() & 3u) != 0) {
        out.push_back(byte{0});
    }
}

Error proto(std::string m) {
    return Error{ErrorKind::Protocol, std::move(m), std::nullopt, std::nullopt};
}
Error invalid(std::string m) {
    return Error{ErrorKind::InvalidArgument, std::move(m), std::nullopt, std::nullopt};
}
Error internal(std::string m) {
    return Error{ErrorKind::Internal, std::move(m), std::nullopt, std::nullopt};
}

std::expected<Rules, Error> decode_rules(std::span<const byte> p) {
    if (p.size() != 96) {
        return std::unexpected(proto("RULES must be exactly 96 bytes"));
    }
    if (rd8(p.data()) != 0x84 || rd8(p.data() + 1) != 1) {
        return std::unexpected(proto("RULES type/version mismatch"));
    }
    if (rd16(p.data() + 10) != 0) {
        return std::unexpected(proto("RULES reserved field must be zero"));
    }

    Rules r;
    // The live server writes 32 here (public.tps); protocol.md still calls the
    // field reserved. Accept both zero and non-zero, expose it as public_tps.
    r.public_tps = rd16(p.data() + 2);
    r.width = rd16(p.data() + 4);
    r.height = rd16(p.data() + 6);
    r.sonar_radius_px = rd16(p.data() + 8);
    r.tps = rd32(p.data() + 12);
    r.cooldown_base_us = rd64(p.data() + 16);
    r.cooldown_per_px_us = rd64(p.data() + 24);
    r.public_delay_ms = rd64(p.data() + 32);
    r.public_period_ms = rd64(p.data() + 40);
    r.event_start_unix_ms = rd64(p.data() + 48);
    r.event_end_unix_ms = rd64(p.data() + 56);
    r.max_slots = rd32(p.data() + 64);
    r.max_paints_per_message = rd32(p.data() + 68);
    r.msg_rate = rd32(p.data() + 72);
    r.msg_burst = rd32(p.data() + 76);
    r.max_message_bytes = rd32(p.data() + 80);
    r.max_payload_bytes = rd32(p.data() + 84);
    r.server_time_us = rd64(p.data() + 88);

    if (r.width == 0 || r.height == 0) {
        return std::unexpected(proto("RULES board dimensions must be non-zero"));
    }
    if (r.tps == 0) {
        return std::unexpected(proto("RULES tps must be non-zero"));
    }
    if (r.max_slots == 0 || r.max_slots > 65536) {
        return std::unexpected(proto("RULES max_slots out of range"));
    }
    if (r.max_message_bytes == 0 || r.max_payload_bytes == 0) {
        return std::unexpected(proto("RULES byte limits must be non-zero"));
    }
    if (r.sonar_radius_px > 16) {
        return std::unexpected(proto("RULES sonar radius out of range"));
    }
    return r;
}

std::expected<Tick, Error> decode_tick(std::span<const byte> p, const DecodeLimits& lim) {
    if (p.size() < 32) {
        return std::unexpected(proto("TICK header truncated"));
    }
    if (rd8(p.data()) != 0x81 || rd8(p.data() + 1) != 1) {
        return std::unexpected(proto("TICK type/version mismatch"));
    }
    if (rd32(p.data() + 28) != 0) {
        return std::unexpected(proto("TICK reserved field must be zero"));
    }

    const std::uint32_t R = rd16(p.data() + 2);
    const std::uint32_t nr = rd32(p.data() + 4);
    const std::uint64_t tick = rd64(p.data() + 8);
    const std::uint32_t nb = rd32(p.data() + 16);
    const std::uint32_t nk = rd32(p.data() + 20);
    const std::uint32_t ns = rd32(p.data() + 24);

    if (R > 16) {
        return std::unexpected(proto("TICK sonar radius out of range"));
    }
    if (nr > lim.max_tick_records) {
        return std::unexpected(proto("TICK too many results"));
    }
    if (nb > lim.max_slots) {
        return std::unexpected(proto("TICK too many brush entries"));
    }
    if (ns > lim.max_slots) {
        return std::unexpected(proto("TICK too many lost slots"));
    }

    const std::uint64_t A = 2ull * R * R + 2ull * R + 1ull;
    if (A != 0 && nk > lim.max_sonar_cells / static_cast<std::uint64_t>(A)) {
        return std::unexpected(proto("TICK too many sonar cells"));
    }

    std::uint64_t off = 32;
    const std::uint64_t brush_t = off;
    off += 8ull * nb;
    const std::uint64_t brush_idx = off;
    off += 4ull * nb;
    const std::uint64_t brush_slot = off;
    off += pad4(static_cast<std::size_t>(2ull * nb));
    const std::uint64_t status = off;
    off += pad4(static_cast<std::size_t>(nr));
    const std::uint64_t values = off;
    off += 4ull * nr;
    const std::uint64_t sonar_idx = off;
    off += 4ull * nk;
    const std::uint64_t sonar_color = off;
    off += 4ull * A * nk;
    const std::uint64_t lost = off;
    off += pad4(static_cast<std::size_t>(2ull * ns));

    if (off != p.size()) {
        return std::unexpected(proto("TICK length mismatch"));
    }

    Tick out;
    out.number = tick;
    out.results.reserve(nr);
    out.brushes.reserve(nb);
    out.sonars.reserve(nk);
    out.lost.reserve(ns);

    std::uint16_t prev_slot = 0;
    bool first = true;
    for (std::uint32_t i = 0; i < nb; ++i) {
        const std::uint16_t slot =
            rd16(p.data() + brush_slot + static_cast<std::uint64_t>(2) * i);
        if (!first && slot <= prev_slot) {
            return std::unexpected(proto("TICK brush.slot must be strictly increasing"));
        }
        first = false;
        prev_slot = slot;
        if (slot >= lim.max_slots) {
            return std::unexpected(proto("TICK brush.slot out of range"));
        }
        const std::uint64_t t = rd64(p.data() + brush_t + 8ull * i);
        const std::uint32_t idx = rd32(p.data() + brush_idx + 4ull * i);
        if (t == 0 && idx != 0) {
            return std::unexpected(proto("TICK empty brush must have idx 0"));
        }
        if (lim.width != 0 && lim.height != 0 &&
            static_cast<std::uint64_t>(idx) >=
                static_cast<std::uint64_t>(lim.width) * lim.height) {
            return std::unexpected(proto("TICK brush.idx out of range"));
        }
        BrushUpdate u;
        u.slot = slot;
        if (t != 0) {
            u.brush.idx = idx;
        }
        u.brush.t_us = t;
        out.brushes.push_back(std::move(u));
    }

    for (std::uint32_t i = 0; i < nr; ++i) {
        const std::uint8_t st = rd8(p.data() + status + i);
        if (st > 6) {
            return std::unexpected(proto("TICK result status out of range"));
        }
        Result r;
        r.code = static_cast<ResultCode>(st);
        r.value = rd32(p.data() + values + 4ull * i);
        r.kind = RecordKind::Paint;
        out.results.push_back(r);
    }

    for (std::uint32_t k = 0; k < nk; ++k) {
        Sonar s;
        s.radius = static_cast<std::uint8_t>(R);
        s.center = rd32(p.data() + sonar_idx + 4ull * k);
        if (lim.width != 0 && lim.height != 0 &&
            static_cast<std::uint64_t>(s.center) >=
                static_cast<std::uint64_t>(lim.width) * lim.height) {
            return std::unexpected(proto("TICK sonar.idx out of range"));
        }
        s.colors.resize(static_cast<std::size_t>(A));
        for (std::uint64_t j = 0; j < A; ++j) {
            const std::uint32_t c =
                rd32(p.data() + sonar_color + 4ull * (static_cast<std::uint64_t>(k) * A + j));
            if (c != 0 && (c & 0xFF000000u) != 0xFF000000u) {
                return std::unexpected(proto("TICK sonar color has alpha other than 255"));
            }
            s.colors[static_cast<std::size_t>(j)] = Color{c};
        }
        out.sonars.push_back(std::move(s));
    }

    prev_slot = 0;
    first = true;
    for (std::uint32_t i = 0; i < ns; ++i) {
        const std::uint16_t slot = rd16(p.data() + lost + 2ull * i);
        if (!first && slot <= prev_slot) {
            return std::unexpected(proto("TICK lost.slot must be strictly increasing"));
        }
        first = false;
        prev_slot = slot;
        if (slot >= lim.max_slots) {
            return std::unexpected(proto("TICK lost.slot out of range"));
        }
        out.lost.push_back(slot);
    }

    return out;
}

std::expected<Snapshot, Error> decode_snapshot(std::span<const byte> p, const DecodeLimits& lim) {
    if (p.size() < 16) {
        return std::unexpected(proto("SNAPSHOT header truncated"));
    }
    if (rd8(p.data()) != 0x83 || rd8(p.data() + 1) != 1) {
        return std::unexpected(proto("SNAPSHOT type/version mismatch"));
    }
    const std::uint8_t codec = rd8(p.data() + 2);
    if (codec > 1) {
        return std::unexpected(proto("SNAPSHOT codec out of range"));
    }
    if (rd8(p.data() + 3) != 0) {
        return std::unexpected(proto("SNAPSHOT reserved field must be zero"));
    }

    Snapshot s;
    s.width = rd16(p.data() + 4);
    s.height = rd16(p.data() + 6);
    s.version = rd64(p.data() + 8);
    s.codec = codec == 0 ? SnapshotCodec::Full : SnapshotCodec::FullWithHistory;

    if (s.width == 0 || s.height == 0) {
        return std::unexpected(proto("SNAPSHOT dimensions must be non-zero"));
    }
    const std::uint64_t count = static_cast<std::uint64_t>(s.width) * s.height;
    if (count > lim.max_entries) {
        return std::unexpected(proto("SNAPSHOT too many pixels"));
    }
    const std::uint64_t expected =
        16ull + (codec == 0 ? 4ull : 12ull) * count;
    if (expected != p.size()) {
        return std::unexpected(proto("SNAPSHOT length mismatch"));
    }

    s.pixels.resize(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint32_t c = rd32(p.data() + 16 + 4ull * i);
        if ((c & 0xFF000000u) != 0xFF000000u) {
            return std::unexpected(proto("SNAPSHOT pixel alpha other than 255"));
        }
        s.pixels[static_cast<std::size_t>(i)] = Color{c};
    }

    if (codec == 1) {
        s.l1.resize(static_cast<std::size_t>(count));
        s.l2.resize(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint32_t a = rd32(p.data() + 16 + 4ull * count + 4ull * i);
            const std::uint32_t b = rd32(p.data() + 16 + 8ull * count + 4ull * i);
            if (a != 0 && (a & 0xFF000000u) != 0xFF000000u) {
                return std::unexpected(proto("SNAPSHOT l1 alpha other than 255"));
            }
            if (b != 0 && (b & 0xFF000000u) != 0xFF000000u) {
                return std::unexpected(proto("SNAPSHOT l2 alpha other than 255"));
            }
            s.l1[static_cast<std::size_t>(i)] = Color{a};
            s.l2[static_cast<std::size_t>(i)] = Color{b};
        }
    }

    return s;
}

std::expected<PublicBatch, Error> decode_public(std::span<const byte> p, const DecodeLimits& lim) {
    if (p.size() < 36) {
        return std::unexpected(proto("PUBLIC header truncated"));
    }
    if (rd8(p.data()) != 0x82 || rd8(p.data() + 1) != 1) {
        return std::unexpected(proto("PUBLIC type/version mismatch"));
    }

    const std::uint16_t sample_rate = rd16(p.data() + 2);
    const std::uint32_t n = rd32(p.data() + 4);
    const std::uint64_t from = rd64(p.data() + 8);
    const std::uint64_t to = rd64(p.data() + 16);
    const std::uint32_t m = rd32(p.data() + 24);
    const std::uint32_t e = rd32(p.data() + 28);
    const std::uint32_t order = rd32(p.data() + 32);

    if (order > 1) {
        return std::unexpected(proto("PUBLIC order out of range"));
    }
    if (from >= to) {
        return std::unexpected(proto("PUBLIC from must be < to"));
    }
    if (n > lim.max_entries) {
        return std::unexpected(proto("PUBLIC too many entries"));
    }
    if (m > lim.max_segments) {
        return std::unexpected(proto("PUBLIC too many segments"));
    }
    if (e > n) {
        return std::unexpected(proto("PUBLIC too many big deltas"));
    }

    const std::uint64_t seg_tick = 36;
    const std::uint64_t seg_n = seg_tick + 4ull * m;
    const std::uint64_t dsmall = seg_n + 4ull * m;
    const std::uint64_t dbig = dsmall + pad4(static_cast<std::size_t>(n));
    const std::uint64_t color = dbig + 4ull * e;
    const std::uint64_t total = color + 4ull * n;
    if (total != p.size()) {
        return std::unexpected(proto("PUBLIC length mismatch"));
    }

    std::uint64_t sum_n = 0;
    std::uint64_t prev_tick = 0;
    bool first_tick = true;
    for (std::uint32_t j = 0; j < m; ++j) {
        const std::uint32_t st = rd32(p.data() + seg_tick + 4ull * j);
        const std::uint32_t sn = rd32(p.data() + seg_n + 4ull * j);
        if (sn > n - sum_n) {
            return std::unexpected(proto("PUBLIC segment entry count overflow"));
        }
        sum_n += sn;
        if (!first_tick && st <= prev_tick) {
            return std::unexpected(proto("PUBLIC seg.tick must be strictly increasing"));
        }
        first_tick = false;
        prev_tick = st;
        if (st > std::numeric_limits<std::uint64_t>::max() - from) {
            return std::unexpected(proto("PUBLIC tick overflow"));
        }
        if (from + st > to) {
            return std::unexpected(proto("PUBLIC segment extends past to"));
        }
    }
    if (sum_n != n) {
        return std::unexpected(proto("PUBLIC segment counts do not sum to n"));
    }

    std::uint32_t count255 = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (rd8(p.data() + dsmall + i) == 255) {
            ++count255;
        }
    }
    if (count255 != e) {
        return std::unexpected(proto("PUBLIC big-delta count mismatch"));
    }

    PublicBatch out;
    out.sample_rate = sample_rate;
    out.from = from;
    out.to = to;
    out.order = order;
    out.segments.reserve(m);

    std::uint32_t dbig_i = 0;
    std::uint32_t entry_i = 0;
    for (std::uint32_t j = 0; j < m; ++j) {
        PublicSegment seg;
        seg.tick = rd32(p.data() + seg_tick + 4ull * j);
        seg.entries.reserve(rd32(p.data() + seg_n + 4ull * j));
        const std::uint32_t sn = rd32(p.data() + seg_n + 4ull * j);

        std::uint32_t key = 0;
        for (std::uint32_t t = 0; t < sn; ++t) {
            std::uint32_t delta = rd8(p.data() + dsmall + entry_i);
            if (delta == 255) {
                if (dbig_i >= e) {
                    return std::unexpected(proto("PUBLIC missing big delta"));
                }
                delta = rd32(p.data() + dbig + 4ull * dbig_i);
                ++dbig_i;
            }
            if (t == 0) {
                key = delta;
            } else {
                if (delta == 0) {
                    return std::unexpected(proto("PUBLIC duplicate pixel key"));
                }
                if (key > std::numeric_limits<std::uint32_t>::max() - delta) {
                    return std::unexpected(proto("PUBLIC key overflow"));
                }
                key += delta;
            }

            const std::uint32_t c = rd32(p.data() + color + 4ull * entry_i);
            if (c > 2 && (c & 0xFF000000u) != 0xFF000000u) {
                return std::unexpected(proto("PUBLIC color has alpha other than 255"));
            }
            seg.entries.push_back(PublicEntry{key, c});
            ++entry_i;
        }
        out.segments.push_back(std::move(seg));
    }

    if (entry_i != n || dbig_i != e) {
        return std::unexpected(proto("PUBLIC trailing column data"));
    }

    return out;
}

}  // namespace

std::optional<PaintToken> PaintToken::from_uuid(std::string_view text) noexcept {
    std::array<char, 32> hex{};
    std::size_t n = 0;
    for (char c : text) {
        if (c == '-') {
            continue;
        }
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper) {
            return std::nullopt;
        }
        if (n >= hex.size()) {
            return std::nullopt;
        }
        hex[n++] = c;
    }
    if (n != hex.size()) {
        return std::nullopt;
    }

    auto value = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
        return static_cast<std::uint8_t>(c - 'A' + 10);
    };

    PaintToken token;
    for (std::size_t i = 0; i < 16; ++i) {
        token.bytes[i] = static_cast<byte>(
            static_cast<std::uint8_t>((value(hex[2 * i]) << 4) | value(hex[2 * i + 1])));
    }
    return token;
}

std::string PaintToken::to_uuid() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            s.push_back('-');
        }
        const auto v = std::to_integer<std::uint8_t>(bytes[i]);
        s.push_back(kHex[v >> 4]);
        s.push_back(kHex[v & 0x0Fu]);
    }
    return s;
}

Batch& Batch::bind(SlotId slot, PaintToken token) {
    binds_.push_back(BindRecord{slot, token, RecordKind::Bind});
    return *this;
}

Batch& Batch::unbind(SlotId slot) {
    binds_.push_back(BindRecord{slot, PaintToken{}, RecordKind::Unbind});
    return *this;
}

Batch& Batch::paint(SlotId slot, PixelIndex idx, Color color) {
    paints_.push_back(PaintRecord{slot, idx, color});
    return *this;
}

void Batch::clear() noexcept {
    binds_.clear();
    paints_.clear();
}

std::vector<RecordMeta> Batch::wire_records() const {
    std::vector<RecordMeta> out;
    out.reserve(size());
    for (const auto& b : binds_) {
        out.push_back(RecordMeta{b.kind, b.slot});
    }
    for (const auto& p : paints_) {
        out.push_back(RecordMeta{RecordKind::Paint, p.slot});
    }
    return out;
}

namespace codec {

std::expected<std::vector<std::byte>, Error>
encode_batch(const Batch& batch, const Rules& rules, int level) {
    const auto binds = batch.bind_records();
    const auto paints = batch.paint_records();

    if (binds.size() > rules.max_slots) {
        return std::unexpected(invalid("too many bind records for max_slots"));
    }
    if (paints.size() > rules.max_paints_per_message) {
        return std::unexpected(invalid("too many paint records for one message"));
    }
    if (rules.width != 0 && rules.height != 0) {
        const std::uint64_t pixels =
            static_cast<std::uint64_t>(rules.width) * rules.height;
        for (const auto& p : paints) {
            if (p.idx >= pixels) {
                return std::unexpected(invalid("paint idx out of board range"));
            }
        }
    }
    for (const auto& b : binds) {
        if (b.slot >= rules.max_slots) {
            return std::unexpected(invalid("bind slot out of range"));
        }
    }
    for (const auto& p : paints) {
        if (p.slot >= rules.max_slots) {
            return std::unexpected(invalid("paint slot out of range"));
        }
        if ((p.color.v & 0xFF000000u) != 0xFF000000u) {
            return std::unexpected(invalid("paint color alpha must be 255"));
        }
    }

    const std::size_t nb = binds.size();
    const std::size_t np = paints.size();
    const std::size_t payload_size =
        16 + pad4(2 * nb) + 16 * nb + pad4(2 * np) + 8 * np;

    std::vector<byte> payload;
    payload.reserve(payload_size);

    put8(payload, 0x01);
    put8(payload, 1);
    put16(payload, 0);
    put32(payload, static_cast<std::uint32_t>(nb));
    put32(payload, static_cast<std::uint32_t>(np));
    put32(payload, 0);

    for (const auto& b : binds) {
        put16(payload, b.slot);
    }
    pad_to4(payload);
    for (const auto& b : binds) {
        payload.insert(payload.end(), b.token.bytes.begin(), b.token.bytes.end());
    }
    for (const auto& p : paints) {
        put16(payload, p.slot);
    }
    pad_to4(payload);
    for (const auto& p : paints) {
        put32(payload, p.idx);
    }
    for (const auto& p : paints) {
        put32(payload, p.color.v);
    }

    if (payload.size() != payload_size) {
        return std::unexpected(internal("BATCH payload size mismatch"));
    }
    if (rules.max_payload_bytes != 0 && payload.size() > rules.max_payload_bytes) {
        return std::unexpected(invalid("BATCH payload exceeds max_payload_bytes"));
    }

    std::vector<byte> frame(ZSTD_compressBound(payload.size()));
    const std::size_t written = ZSTD_compress(
        frame.data(), frame.size(), payload.data(), payload.size(), level);
    if (ZSTD_isError(written)) {
        return std::unexpected(internal(std::string("zstd compress failed: ") +
                                       ZSTD_getErrorName(written)));
    }
    if (rules.max_message_bytes != 0 && written > rules.max_message_bytes) {
        return std::unexpected(invalid("compressed BATCH exceeds max_message_bytes"));
    }
    frame.resize(written);
    return frame;
}

std::expected<DecodedMessage, Error>
decode_frame(std::span<const std::byte> frame, const DecodeLimits& limits) {
    if (frame.empty()) {
        return std::unexpected(proto("empty WebSocket message"));
    }
    if (frame.size() > limits.max_message_bytes) {
        return std::unexpected(proto("compressed message exceeds max_message_bytes"));
    }

    const std::size_t frame_size =
        ZSTD_findFrameCompressedSize(frame.data(), frame.size());
    if (ZSTD_isError(frame_size) || frame_size != frame.size()) {
        return std::unexpected(proto("message must be exactly one zstd frame"));
    }

    ZSTD_frameHeader header{};
    const std::size_t header_status =
        ZSTD_getFrameHeader(&header, frame.data(), frame.size());
    if (ZSTD_isError(header_status) || header_status != 0) {
        return std::unexpected(proto("invalid zstd frame header"));
    }
    if (header.dictID != 0) {
        return std::unexpected(proto("zstd dictionary is not allowed"));
    }
    if (header.windowSize > 8ull * 1024ull * 1024ull) {
        return std::unexpected(proto("zstd window size exceeds 8 MiB"));
    }

    // Downlink SNAPSHOT can be much larger than the client->server 1 MiB
    // limit (a 1000x600 codec-1 snapshot is 7,200,016 bytes). Use the frame's
    // declared content size when available so TICKs do not allocate the cap.
    std::size_t capacity = limits.max_payload_bytes;
    const unsigned long long content =
        ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (content != ZSTD_CONTENTSIZE_UNKNOWN && content != ZSTD_CONTENTSIZE_ERROR) {
        if (content > limits.max_payload_bytes) {
            return std::unexpected(proto("frame content exceeds max_payload_bytes"));
        }
        capacity = static_cast<std::size_t>(content);
    }
    if (capacity < 2) {
        capacity = 2;
    }
    std::vector<byte> payload(capacity);
    const std::size_t written = ZSTD_decompress(
        payload.data(), payload.size(), frame.data(), frame.size());
    if (ZSTD_isError(written)) {
        return std::unexpected(proto(std::string("zstd decompress failed: ") +
                                     ZSTD_getErrorName(written)));
    }
    payload.resize(written);
    return decode_payload(payload, limits);
}

std::expected<DecodedMessage, Error>
decode_payload(std::span<const std::byte> payload, const DecodeLimits& limits) {
    if (payload.size() < 2) {
        return std::unexpected(proto("payload truncated"));
    }
    if (payload.size() > limits.max_payload_bytes) {
        return std::unexpected(proto("payload exceeds max_payload_bytes"));
    }

    const std::uint8_t type = rd8(payload.data());
    const std::uint8_t version = rd8(payload.data() + 1);
    if (version != 1) {
        return std::unexpected(proto("unsupported payload version"));
    }

    switch (type) {
        case 0x84: {
            auto r = decode_rules(payload);
            if (!r) return std::unexpected(std::move(r.error()));
            return DecodedMessage{std::move(*r)};
        }
        case 0x81: {
            auto t = decode_tick(payload, limits);
            if (!t) return std::unexpected(std::move(t.error()));
            return DecodedMessage{std::move(*t)};
        }
        case 0x83: {
            auto s = decode_snapshot(payload, limits);
            if (!s) return std::unexpected(std::move(s.error()));
            return DecodedMessage{std::move(*s)};
        }
        case 0x82: {
            auto b = decode_public(payload, limits);
            if (!b) return std::unexpected(std::move(b.error()));
            return DecodedMessage{std::move(*b)};
        }
        default:
            return DecodedMessage{UnknownMessage{type}};
    }
}

}  // namespace codec
}  // namespace pb2027
