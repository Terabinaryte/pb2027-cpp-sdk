#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <variant>
#include <vector>

#include "pb2027/error.hpp"
#include "pb2027/types.hpp"

namespace pb2027 {

struct RecordMeta {
    RecordKind kind = RecordKind::Paint;
    SlotId slot = 0;
};

// A batch is a logical list of records. Encoding always puts all bind/unbind
// records first (they share the wire "bind" columns), then all paints.
class Batch {
public:
    struct BindRecord {
        SlotId slot = 0;
        PaintToken token{};
        RecordKind kind = RecordKind::Bind;
    };
    struct PaintRecord {
        SlotId slot = 0;
        PixelIndex idx = 0;
        Color color{};
    };

    Batch& bind(SlotId slot, PaintToken token);
    Batch& unbind(SlotId slot);
    Batch& paint(SlotId slot, PixelIndex idx, Color color);

    std::size_t size() const noexcept { return binds_.size() + paints_.size(); }
    bool empty() const noexcept { return size() == 0; }
    void clear() noexcept;

    // Wire order: bind/unbind records first, then paint records.
    std::vector<RecordMeta> wire_records() const;

    std::span<const BindRecord> bind_records() const noexcept { return binds_; }
    std::span<const PaintRecord> paint_records() const noexcept { return paints_; }

private:
    std::vector<BindRecord> binds_;
    std::vector<PaintRecord> paints_;
};

namespace codec {

inline constexpr std::uint32_t kDefaultMaxBytes = 1u << 20;

struct DecodeLimits {
    std::uint32_t max_message_bytes = kDefaultMaxBytes;
    std::uint32_t max_payload_bytes = kDefaultMaxBytes;
    std::uint32_t max_slots = 65536;
    std::uint32_t max_segments = 1u << 16;
    std::uint32_t max_entries = 1u << 20;
    std::uint32_t max_sonar_cells = 1u << 20;
    std::uint32_t max_tick_records = 1u << 20;
    // TICK/SNAPSHOT use these for pixel range checks; 0 means "unknown, skip".
    // PUBLIC keeps wire keys and does not need them (see Board).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

using DecodedMessage =
    std::variant<Rules, Snapshot, PublicBatch, Tick, UnknownMessage>;

// Encodes a BATCH as exactly one zstd frame. Validates against `rules`.
// On error nothing is sent; the Error is InvalidArgument or Internal.
std::expected<std::vector<std::byte>, Error>
encode_batch(const Batch& batch, const Rules& rules, int level = 3);

// Decompresses exactly one zstd frame and decodes the payload.
std::expected<DecodedMessage, Error>
decode_frame(std::span<const std::byte> frame, const DecodeLimits& limits);

// Decodes an already decompressed payload (e.g. a concatenated /api/board body
// after splitting it into messages).
std::expected<DecodedMessage, Error>
decode_payload(std::span<const std::byte> payload, const DecodeLimits& limits);

}  // namespace codec

}  // namespace pb2027
