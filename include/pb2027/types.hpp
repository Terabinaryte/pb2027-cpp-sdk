#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pb2027 {

using Uid = std::uint32_t;
using SlotId = std::uint16_t;      // wire is u16; max_slots > 65535 is meaningless
using PixelIndex = std::uint32_t;
using TickNumber = std::uint64_t;

struct SubmissionId {
    std::uint64_t value = 0;
};

struct Color {
    std::uint32_t v = 0;

    static constexpr Color rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
        return Color{static_cast<std::uint32_t>(r) |
                     (static_cast<std::uint32_t>(g) << 8) |
                     (static_cast<std::uint32_t>(b) << 16) | 0xFF000000u};
    }

    constexpr std::uint8_t r() const noexcept {
        return static_cast<std::uint8_t>(v & 0xFFu);
    }
    constexpr std::uint8_t g() const noexcept {
        return static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    }
    constexpr std::uint8_t b() const noexcept {
        return static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    }
};

struct PaintToken {
    std::array<std::byte, 16> bytes{};

    // Accepts the lowercase hyphenated UUID form from the protocol, as well as
    // the 32-hex-digit form. Returns nullopt on malformed input.
    static std::optional<PaintToken> from_uuid(std::string_view text) noexcept;
    std::string to_uuid() const;

    friend bool operator==(const PaintToken&, const PaintToken&) = default;
};

struct Rules {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t public_tps = 32;  // RULES offset 2; protocol.md calls it reserved
    std::uint16_t sonar_radius_px = 0;
    std::uint32_t tps = 0;
    std::uint64_t cooldown_base_us = 0;
    std::uint64_t cooldown_per_px_us = 0;
    std::uint64_t public_delay_ms = 0;
    std::uint64_t public_period_ms = 0;
    std::uint64_t event_start_unix_ms = 0;
    std::uint64_t event_end_unix_ms = 0;
    std::uint32_t max_slots = 0;
    std::uint32_t max_paints_per_message = 0;
    std::uint32_t msg_rate = 0;
    std::uint32_t msg_burst = 0;
    std::uint32_t max_message_bytes = 0;
    std::uint32_t max_payload_bytes = 0;
    std::uint64_t server_time_us = 0;  // encoded server time (UNIX us)
};

enum class ResultCode : std::uint8_t {
    Ok = 0,
    Cooling = 1,
    NoPaintToken = 2,
    BadPaintToken = 3,
    Banned = 4,
    Closed = 5,
    IpLimit = 6,
};

enum class RecordKind : std::uint8_t { Bind, Unbind, Paint };

struct Result {
    ResultCode code = ResultCode::Ok;
    std::uint32_t value = 0;   // Cooling: wait us; IpLimit: limit; else 0
    RecordKind kind = RecordKind::Paint;
    SlotId slot = 0;
    SubmissionId submission{};
    std::uint32_t record = 0;  // index inside the submission (wire order)
};

struct Brush {
    std::optional<PixelIndex> idx;  // nullopt = empty brush
    std::uint64_t t_us = 0;         // server accept time (UNIX us)
};

struct BrushUpdate {
    SlotId slot = 0;
    Brush brush;
};

struct Sonar {
    PixelIndex center = 0;
    std::uint8_t radius = 0;
    std::vector<Color> colors;  // diamond order; out-of-board cells are 0
};

struct Tick {
    TickNumber number = 0;
    std::vector<Result> results;
    std::vector<BrushUpdate> brushes;
    std::vector<Sonar> sonars;
    std::vector<SlotId> lost;
};

enum class SnapshotCodec : std::uint8_t {
    Full = 0,             // current pixels only
    FullWithHistory = 1,  // pixels + l1/l2
};

struct Snapshot {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint64_t version = 0;
    SnapshotCodec codec = SnapshotCodec::Full;
    std::vector<Color> pixels;
    std::vector<Color> l1;
    std::vector<Color> l2;
};

// key is the wire key. The Board converts it to a pixel index because only the
// Board knows the canvas dimensions (PUBLIC order=1 needs H).
struct PublicEntry {
    std::uint32_t key = 0;
    std::uint32_t color = 0;  // 0 -> l1, 1 -> l2, 2 -> unchanged, else RGBA
};

struct PublicSegment {
    std::uint32_t tick = 0;  // relative to PublicBatch::from
    std::vector<PublicEntry> entries;
};

struct PublicBatch {
    std::uint16_t sample_rate = 0;
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    std::uint32_t order = 0;  // 0 row-major, 1 column-major
    std::vector<PublicSegment> segments;
};

struct UnknownMessage {
    std::uint8_t type = 0;
};

struct ProxyConfig {
    std::string host;
    std::uint16_t port = 0;
};

}  // namespace pb2027
