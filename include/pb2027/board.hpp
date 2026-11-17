#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "pb2027/error.hpp"
#include "pb2027/types.hpp"

namespace pb2027 {

// Public canvas plus the two history layers required by protocol §3.7.3.
// Board owns no network state: it consumes structured Snapshot/PublicBatch
// values from the codec and Sonar values from TICK messages.
//
// Dimensions come from apply(Snapshot) (Option B in PLAN §5.7) or from
// reset(), which pre-sizes the canvas from RULES/meta before a Snapshot
// arrives. A Board is move-only and is not thread-safe.
enum class ColorSource {
    Public,
    Sonar,
};

struct ResolvedColor {
    Color color{};
    ColorSource source = ColorSource::Public;
    TickNumber tick = 0;
};

class Board {
public:
    Board();
    ~Board();
    Board(Board&&) noexcept;

    // Clears the canvas, the history layers and the sonar table, and sets the
    // size. A zero width or height clears the Board back to the uninitialized
    // state (initialized() == false).
    void reset(std::uint16_t width, std::uint16_t height);

    // Rebuilds the canvas from the snapshot (the snapshot is the authority for
    // width/height), clears the sonar table and sets version(). The payload
    // size must be width*height. codec == Full requires empty l1/l2;
    // codec == FullWithHistory requires both layers to be exactly
    // width*height. History cells written as 0 by a SNAPSHOT ("empty",
    // protocol §3.7.2) are normalized to the pixel's current color, which is
    // how "empty" is represented by the §3.7.3 pseudo-code.
    std::expected<void, Error> apply(const Snapshot&);

    // Applies a decoded PUBLIC batch. Requires initialized(). The wire key is
    // converted to a pixel index with this Board's width/height (order=0 is
    // row-major, order=1 is column-major, protocol §3.7.3). The whole batch is
    // validated before anything is mutated, so on error the Board is
    // unchanged. After segment j version() == from + seg.tick; after the last
    // segment version() == to.
    //
    // Color codes 0/1 refer to the history layers. If the Board was only
    // reset() (or received a codec=Full SNAPSHOT) there are no history layers
    // and such entries return InvalidArgument.
    std::expected<void, Error> apply(const PublicBatch&);

    // Adds a TICK sonar to the overlay table K (protocol §3.8). Cells whose
    // color is 0 (the out-of-board sentinel from §3.7.4) are skipped. No-op
    // until the Board is initialized.
    void overlay(const Sonar&, TickNumber);

    // Convenience for a painting connection: overlays every Sonar in `tick`
    // tagged with tick.number, then prunes entries that the public canvas has
    // already caught up with (tick <= version()).
    void observe(const Tick& tick);

    // Public canvas color only, ignoring the sonar overlay.  Defense detection
    // must use this: a recent sonar entry can be newer than the delayed public
    // version and would otherwise mask an attack that happened after it.
    std::optional<Color> public_color_at(PixelIndex) const;

    // Explicit two-layer resolution.  The sonar entry wins only when its tick
    // is at least `min_sonar_tick`; otherwise the delayed public canvas is
    // used.  The returned source/tick tell the caller which layer answered.
    // Prefer this over any implicit "visible" merge: the public version is
    // delayed, so a recent sonar entry can be newer than public while already
    // stale relative to the board's current state.
    std::optional<ResolvedColor> resolve_color(
        PixelIndex idx, TickNumber min_sonar_tick) const;

    // Drops overlay entries whose tick <= public_version (protocol §3.9).
    void prune(TickNumber public_version);

    bool initialized() const noexcept;
    std::uint16_t width() const noexcept;
    std::uint16_t height() const noexcept;
    TickNumber version() const noexcept;

    // Public canvas color, or nullopt when the Board is uninitialized or the
    // index is out of range. The sonar overlay is not merged in here; it is
    // exposed through sonar_at() and the caller decides whether it wins
    // (protocol §3.9 rendering rule: K first, then the public canvas).
    std::optional<Color> color_at(PixelIndex) const;

    // Overlay entry (color, tick), or nullopt when there is no live entry or
    // the index is out of range.
    std::optional<std::pair<Color, TickNumber>> sonar_at(PixelIndex) const;

    // Span over the public canvas, width()*height() elements. It is
    // invalidated by reset() or apply(Snapshot), which may change the size;
    // apply(PublicBatch) does not reallocate it. Empty when uninitialized.
    std::span<const Color> pixels() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Sonar overlay table K from protocol §3.8/§3.9: pixel index -> (color, tick),
// stored as a color array, a tick array and a FIFO of (pixel, tick) so that
// pruning never scans the whole canvas.
//
// add() has to translate a Sonar's center plus its diamond offsets into pixel
// indices, which needs the canvas dimensions. Construct with (width, height)
// or call reset(); a default-constructed table is 0x0 and ignores add().
class SonarTable {
public:
    SonarTable();
    SonarTable(std::uint16_t width, std::uint16_t height);
    ~SonarTable();
    SonarTable(SonarTable&&) noexcept;

    // Clears all entries and sets the canvas dimensions.
    void reset(std::uint16_t width, std::uint16_t height);

    // `sonar.colors` must hold 2*r^2 + 2*r + 1 cells in diamond order
    // (§3.7.4). Entries whose color is 0 (the out-of-board sentinel) are
    // skipped, as are cells that fall outside the canvas. A sonar with a
    // malformed cell count or an out-of-range center is ignored.
    void add(const Sonar&, TickNumber);

    // Removes entries whose tick <= public_version.
    void prune(TickNumber);

    std::optional<std::pair<Color, TickNumber>> at(PixelIndex) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pb2027
