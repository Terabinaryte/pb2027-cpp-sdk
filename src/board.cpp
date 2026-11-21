#include "pb2027/board.hpp"

#include <cstddef>
#include <deque>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace pb2027 {
namespace {

constexpr TickNumber kNoTick = std::numeric_limits<TickNumber>::max();

Error invalid(std::string message) {
    return Error{ErrorKind::InvalidArgument, std::move(message), std::nullopt, std::nullopt};
}
Error proto(std::string message) {
    return Error{ErrorKind::Protocol, std::move(message), std::nullopt, std::nullopt};
}

std::uint64_t pixel_count(std::uint16_t width, std::uint16_t height) noexcept {
    return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
}

// Protocol §3.7.3: order=0 -> key == index; order=1 -> index is
// (key % H) * W + key / H. Keys are always < W*H.
std::optional<PixelIndex> map_key(std::uint32_t key, std::uint32_t order,
                                  std::uint16_t width, std::uint16_t height,
                                  std::uint64_t total) noexcept {
    const std::uint64_t k = key;
    if (k >= total) {
        return std::nullopt;
    }
    if (order == 0) {
        return static_cast<PixelIndex>(k);
    }
    if (height == 0) {
        return std::nullopt;
    }
    const std::uint64_t idx = (k % height) * width + (k / height);
    if (idx >= total) {
        return std::nullopt;
    }
    return static_cast<PixelIndex>(idx);
}

bool valid_color(std::uint32_t color) noexcept {
    return color <= 2 || (color & 0xFF000000u) == 0xFF000000u;
}

}  // namespace

// ---------------------------------------------------------------------------
// SonarTable
// ---------------------------------------------------------------------------

struct SonarTable::Impl {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::vector<Color> colors;
    std::vector<TickNumber> ticks;  // kNoTick = no entry
    std::deque<std::pair<PixelIndex, TickNumber>> fifo;
};


SonarTable::SonarTable() : impl_(std::make_unique<Impl>()) {}

SonarTable::SonarTable(std::uint16_t width, std::uint16_t height)
    : impl_(std::make_unique<Impl>()) {
    reset(width, height);
}

SonarTable::~SonarTable() = default;

SonarTable::SonarTable(SonarTable&&) noexcept = default;

void SonarTable::reset(std::uint16_t width, std::uint16_t height) {
    if (!impl_) {
        return;
    }
    impl_->width = width;
    impl_->height = height;
    const auto count = static_cast<std::size_t>(pixel_count(width, height));
    impl_->colors.assign(count, Color{});
    impl_->ticks.assign(count, kNoTick);
    impl_->fifo.clear();
}

void SonarTable::add(const Sonar& sonar, TickNumber tick) {
    if (!impl_ || impl_->width == 0 || impl_->height == 0) {
        return;
    }

    const std::uint32_t radius = sonar.radius;
    const std::uint64_t cells =
        2ull * radius * radius + 2ull * radius + 1ull;
    if (sonar.colors.size() != cells) {
        return;  // cannot interpret the diamond
    }
    const std::uint64_t total = pixel_count(impl_->width, impl_->height);
    if (static_cast<std::uint64_t>(sonar.center) >= total) {
        return;
    }

    const std::int64_t cx = static_cast<std::int64_t>(sonar.center % impl_->width);
    const std::int64_t cy = static_cast<std::int64_t>(sonar.center / impl_->width);
    const std::int64_t r = static_cast<std::int64_t>(radius);
    std::size_t k = 0;
    for (std::int64_t dy = -r; dy <= r; ++dy) {
        const std::int64_t span = r - (dy < 0 ? -dy : dy);
        for (std::int64_t dx = -span; dx <= span; ++dx, ++k) {
            const Color color = sonar.colors[k];
            // 0 is the out-of-board sentinel; valid colors always have
            // alpha 255 (§3.7.4).
            if (color.v == 0) {
                continue;
            }
            const std::int64_t x = cx + dx;
            const std::int64_t y = cy + dy;
            if (x < 0 || x >= impl_->width || y < 0 || y >= impl_->height) {
                continue;
            }
            const auto idx = static_cast<PixelIndex>(
                static_cast<std::uint64_t>(y) * impl_->width +
                static_cast<std::uint64_t>(x));
            impl_->colors[idx] = color;
            impl_->ticks[idx] = tick;
            impl_->fifo.emplace_back(idx, tick);
        }
    }
}

void SonarTable::prune(TickNumber public_version) {
    if (!impl_) {
        return;
    }
    // Entries are queued in arrival order and TICK numbers are monotonic, so
    // everything that expires sits at the front. A stale FIFO record only
    // clears the pixel if the pixel still holds that exact tick.
    while (!impl_->fifo.empty() && impl_->fifo.front().second <= public_version) {
        const std::pair<PixelIndex, TickNumber> entry = impl_->fifo.front();
        impl_->fifo.pop_front();
        if (impl_->ticks[entry.first] == entry.second) {
            impl_->ticks[entry.first] = kNoTick;
            impl_->colors[entry.first] = Color{};
        }
    }
}

std::optional<std::pair<Color, TickNumber>> SonarTable::at(PixelIndex idx) const {
    if (!impl_ || static_cast<std::size_t>(idx) >= impl_->ticks.size()) {
        return std::nullopt;
    }
    if (impl_->ticks[idx] == kNoTick) {
        return std::nullopt;
    }
    return std::make_pair(impl_->colors[idx], impl_->ticks[idx]);
}

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------

struct Board::Impl {
    bool initialized = false;
    bool has_public = false;   // a full Snapshot has been applied
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    TickNumber version = 0;
    std::vector<Color> pixels;
    std::vector<Color> l1;  // empty vector = history layers unavailable
    std::vector<Color> l2;
    SonarTable sonar;
};

Board::Board() : impl_(std::make_unique<Impl>()) {}

Board::~Board() = default;

Board::Board(Board&&) noexcept = default;

void Board::reset(std::uint16_t width, std::uint16_t height) {
    if (!impl_) {
        return;
    }
    if (width == 0 || height == 0) {
        impl_->initialized = false;
        impl_->has_public = false;
        impl_->width = 0;
        impl_->height = 0;
        impl_->version = 0;
        impl_->pixels.clear();
        impl_->l1.clear();
        impl_->l2.clear();
        impl_->sonar.reset(0, 0);
        return;
    }

    const auto count = static_cast<std::size_t>(pixel_count(width, height));
    impl_->initialized = true;
    impl_->width = width;
    impl_->height = height;
    impl_->version = 0;
    impl_->pixels.assign(count, Color{});
    impl_->l1.clear();
    impl_->l2.clear();
    impl_->has_public = false;
    impl_->sonar.reset(width, height);
}

std::expected<void, Error> Board::apply(const Snapshot& snapshot) {
    if (!impl_) {
        return std::unexpected(invalid("Board is not initialized"));
    }
    if (snapshot.width == 0 || snapshot.height == 0) {
        return std::unexpected(invalid("SNAPSHOT dimensions must be non-zero"));
    }
    if (snapshot.codec != SnapshotCodec::Full &&
        snapshot.codec != SnapshotCodec::FullWithHistory) {
        return std::unexpected(invalid("SNAPSHOT codec must be Full or FullWithHistory"));
    }

    const auto count =
        static_cast<std::size_t>(pixel_count(snapshot.width, snapshot.height));
    if (snapshot.pixels.size() != count) {
        return std::unexpected(invalid("SNAPSHOT pixels must contain width*height colors"));
    }

    const bool with_history = snapshot.codec == SnapshotCodec::FullWithHistory;
    if (with_history) {
        if (snapshot.l1.size() != count || snapshot.l2.size() != count) {
            return std::unexpected(
                invalid("SNAPSHOT history layers must contain width*height colors"));
        }
    } else if (!snapshot.l1.empty() || !snapshot.l2.empty()) {
        return std::unexpected(
            invalid("SNAPSHOT codec Full must not carry history layers"));
    }

    // Build the new state first so a failed validation/allocation leaves the
    // current board untouched.
    std::vector<Color> pixels(snapshot.pixels.begin(), snapshot.pixels.end());
    std::vector<Color> l1;
    std::vector<Color> l2;
    if (with_history) {
        l1.resize(count);
        l2.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            // A SNAPSHOT writes 0 for an empty history cell (§3.7.2); the
            // §3.7.3 model represents "empty" as the pixel's current color.
            l1[i] = snapshot.l1[i].v == 0 ? pixels[i] : snapshot.l1[i];
            l2[i] = snapshot.l2[i].v == 0 ? pixels[i] : snapshot.l2[i];
        }
    }

    impl_->pixels = std::move(pixels);
    impl_->l1 = std::move(l1);
    impl_->l2 = std::move(l2);
    impl_->width = snapshot.width;
    impl_->height = snapshot.height;
    impl_->version = snapshot.version;
    impl_->initialized = true;
    impl_->has_public = true;
    impl_->sonar.reset(snapshot.width, snapshot.height);
    return {};
}

std::expected<void, Error> Board::apply(const PublicBatch& batch) {
    if (!impl_ || !impl_->initialized) {
        return std::unexpected(
            invalid("Board is not initialized; call reset() or apply(Snapshot) first"));
    }
    if (batch.order > 1) {
        return std::unexpected(invalid("PUBLIC order must be 0 or 1"));
    }
    if (batch.from >= batch.to) {
        return std::unexpected(proto("PUBLIC from must be less than to"));
    }

    const std::uint64_t total = pixel_count(impl_->width, impl_->height);
    const bool have_history = !impl_->l1.empty() && !impl_->l2.empty();

    // Pass 1: validate the entire batch (including every key and color) so an
    // error never leaves a partially applied frame behind.
    std::uint64_t prev_tick = 0;
    bool first_segment = true;
    for (const PublicSegment& segment : batch.segments) {
        if (segment.tick > std::numeric_limits<std::uint64_t>::max() - batch.from) {
            return std::unexpected(proto("PUBLIC segment tick overflows from"));
        }
        if (batch.from + segment.tick > batch.to) {
            return std::unexpected(proto("PUBLIC segment extends past to"));
        }
        if (!first_segment && segment.tick <= prev_tick) {
            return std::unexpected(
                proto("PUBLIC segment ticks must be strictly increasing"));
        }
        first_segment = false;
        prev_tick = segment.tick;

        for (const PublicEntry& entry : segment.entries) {
            if (!map_key(entry.key, batch.order, impl_->width, impl_->height, total)) {
                return std::unexpected(invalid("PUBLIC key is outside the canvas"));
            }
            if (!valid_color(entry.color)) {
                return std::unexpected(
                    proto("PUBLIC color must be 0, 1, 2, or have alpha 255"));
            }
            if ((entry.color == 0 || entry.color == 1) && !have_history) {
                return std::unexpected(
                    invalid("PUBLIC color code 0/1 needs history layers; apply a "
                            "FullWithHistory snapshot first"));
            }
        }
    }

    // Pass 2: apply segments in order (§3.7.3).
    for (const PublicSegment& segment : batch.segments) {
        for (const PublicEntry& entry : segment.entries) {
            const PixelIndex idx =
                *map_key(entry.key, batch.order, impl_->width, impl_->height, total);
            const Color old = impl_->pixels[idx];
            Color updated = old;
            if (entry.color == 0) {
                updated = impl_->l1[idx];
            } else if (entry.color == 1) {
                updated = impl_->l2[idx];
            } else if (entry.color != 2) {
                updated = Color{entry.color};
            }
            if (updated.v == old.v) {
                continue;  // in-place redraw: color and history stay untouched
            }
            if (have_history) {
                const Color h1 = impl_->l1[idx];
                const Color h2 = impl_->l2[idx];
                Color history = updated;
                if (h1.v != old.v && h1.v != updated.v) {
                    history = h1;
                } else if (h2.v != old.v && h2.v != updated.v) {
                    history = h2;
                }
                impl_->l2[idx] = history;
                impl_->l1[idx] = old;
            }
            impl_->pixels[idx] = updated;
        }
        impl_->version = batch.from + segment.tick;
    }
    impl_->version = batch.to;
    return {};
}

void Board::overlay(const Sonar& sonar, TickNumber tick) {
    if (!impl_ || !impl_->initialized) {
        return;
    }
    impl_->sonar.add(sonar, tick);
}

void Board::observe(const Tick& tick) {
    if (!impl_ || !impl_->initialized) {
        return;
    }
    for (const Sonar& sonar : tick.sonars) {
        impl_->sonar.add(sonar, tick.number);
    }
    // Bound the local sonar history by observation age, not only by the
    // (possibly stalled or heavily delayed) public version.  Otherwise a
    // painter-only client that never advances the public canvas accumulates
    // one FIFO record per sonar cell forever.
    constexpr TickNumber kRetentionTicks = 1024;  // ~8 s at 128 tps
    if (tick.number > kRetentionTicks) {
        impl_->sonar.prune(tick.number - kRetentionTicks);
    }
    impl_->sonar.prune(impl_->version);
}

std::optional<ResolvedColor> Board::resolve_color(
    PixelIndex idx, TickNumber min_sonar_tick) const {
    if (!impl_ || !impl_->initialized) {
        return std::nullopt;
    }
    if (auto sonar = impl_->sonar.at(idx);
        sonar && sonar->second >= min_sonar_tick) {
        return ResolvedColor{sonar->first, ColorSource::Sonar, sonar->second};
    }
    if (!impl_->has_public ||
        static_cast<std::size_t>(idx) >= impl_->pixels.size()) {
        return std::nullopt;
    }
    return ResolvedColor{impl_->pixels[idx], ColorSource::Public, impl_->version};
}

std::optional<Color> Board::public_color_at(PixelIndex idx) const {
    if (!impl_ || !impl_->initialized || !impl_->has_public ||
        static_cast<std::size_t>(idx) >= impl_->pixels.size()) {
        return std::nullopt;
    }
    return impl_->pixels[idx];
}

void Board::prune(TickNumber public_version) {
    if (!impl_) {
        return;
    }
    impl_->sonar.prune(public_version);
}

bool Board::initialized() const noexcept {
    return impl_ && impl_->initialized;
}

std::uint16_t Board::width() const noexcept {
    return impl_ ? impl_->width : 0;
}

std::uint16_t Board::height() const noexcept {
    return impl_ ? impl_->height : 0;
}

TickNumber Board::version() const noexcept {
    return impl_ ? impl_->version : 0;
}

std::optional<Color> Board::color_at(PixelIndex idx) const {
    if (!impl_ || !impl_->initialized) {
        return std::nullopt;
    }
    if (static_cast<std::size_t>(idx) >= impl_->pixels.size()) {
        return std::nullopt;
    }
    return impl_->pixels[idx];
}

std::optional<std::pair<Color, TickNumber>> Board::sonar_at(PixelIndex idx) const {
    if (!impl_ || !impl_->initialized) {
        return std::nullopt;
    }
    return impl_->sonar.at(idx);
}

std::span<const Color> Board::pixels() const noexcept {
    if (!impl_) {
        return {};
    }
    return impl_->pixels;
}

}  // namespace pb2027
