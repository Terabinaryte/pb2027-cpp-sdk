#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "pb2027/board.hpp"

using namespace pb2027;

namespace {

Color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return Color::rgba(r, g, b);
}

const Color kBase = rgb(10, 20, 30);

Snapshot make_snapshot(std::uint16_t width, std::uint16_t height,
                       std::uint64_t version, SnapshotCodec codec,
                       Color fill = kBase) {
    Snapshot s;
    s.width = width;
    s.height = height;
    s.version = version;
    s.codec = codec;
    s.pixels.assign(static_cast<std::size_t>(width) * height, fill);
    if (codec == SnapshotCodec::FullWithHistory) {
        s.l1.assign(static_cast<std::size_t>(width) * height, Color{});
        s.l2.assign(static_cast<std::size_t>(width) * height, Color{});
    }
    return s;
}

Snapshot one_pixel(std::uint64_t version, Color current,
                   Color l1 = Color{}, Color l2 = Color{}) {
    Snapshot s;
    s.width = 1;
    s.height = 1;
    s.version = version;
    s.codec = SnapshotCodec::FullWithHistory;
    s.pixels = {current};
    s.l1 = {l1};
    s.l2 = {l2};
    return s;
}

PublicEntry entry(std::uint32_t key, Color color) {
    return PublicEntry{key, color.v};
}

PublicEntry code_entry(std::uint32_t key, std::uint32_t code) {
    return PublicEntry{key, code};
}

PublicBatch single_segment(std::uint64_t from, std::uint64_t to,
                           std::uint32_t tick,
                           std::vector<PublicEntry> entries,
                           std::uint32_t order = 0) {
    PublicBatch b;
    b.from = from;
    b.to = to;
    b.order = order;
    PublicSegment seg;
    seg.tick = tick;
    seg.entries = std::move(entries);
    b.segments.push_back(std::move(seg));
    return b;
}

}  // namespace

TEST_CASE("Board starts uninitialized") {
    Board b;
    CHECK_FALSE(b.initialized());
    CHECK(b.width() == 0);
    CHECK(b.height() == 0);
    CHECK(b.version() == 0);
    CHECK(b.pixels().empty());
    CHECK_FALSE(b.color_at(0).has_value());
    CHECK_FALSE(b.sonar_at(0).has_value());

    // Overlay/prune before initialization are no-ops.
    b.overlay(Sonar{0, 0, {rgb(1, 1, 1)}}, 1);
    b.prune(1);
    CHECK_FALSE(b.sonar_at(0).has_value());
}

TEST_CASE("reset clears the canvas, the history layers and the sonar table") {
    Board b;
    b.reset(3, 2);
    REQUIRE(b.initialized());
    CHECK(b.width() == 3);
    CHECK(b.height() == 2);
    CHECK(b.version() == 0);
    REQUIRE(b.pixels().size() == 6);
    for (const Color& px : b.pixels()) {
        CHECK(px.v == 0);
    }
    REQUIRE(b.color_at(5).has_value());
    CHECK(b.color_at(5)->v == 0);
    CHECK_FALSE(b.color_at(6).has_value());

    b.overlay(Sonar{0, 0, {rgb(9, 9, 9)}}, 5);
    REQUIRE(b.sonar_at(0).has_value());
    b.reset(3, 2);
    CHECK_FALSE(b.sonar_at(0).has_value());

    // A zero dimension returns the Board to the uninitialized state.
    b.reset(0, 3);
    CHECK_FALSE(b.initialized());
    CHECK(b.pixels().empty());
    CHECK(b.width() == 0);
}

TEST_CASE("resolve_color is explicit and freshness-gated") {
    Board b;
    b.reset(1, 1);
    Snapshot s;
    s.width = 1;
    s.height = 1;
    s.version = 7;
    s.codec = SnapshotCodec::Full;
    s.pixels = {rgb(1, 2, 3)};
    REQUIRE(b.apply(s).has_value());

    b.overlay(Sonar{0, 0, {rgb(9, 9, 9)}}, 10);

    auto r = b.resolve_color(0, 10);
    REQUIRE(r.has_value());
    CHECK(r->source == ColorSource::Sonar);
    CHECK(r->color.v == rgb(9, 9, 9).v);
    CHECK(r->tick == 10);

    // Too-old sonar is not allowed to mask the public canvas.
    r = b.resolve_color(0, 11);
    REQUIRE(r.has_value());
    CHECK(r->source == ColorSource::Public);
    CHECK(r->color.v == rgb(1, 2, 3).v);
    CHECK(r->tick == 7);

    // Public catches up: prune drops the old sonar, then public answers.
    b.prune(10);
    CHECK_FALSE(b.sonar_at(0).has_value());
    r = b.resolve_color(0, 0);
    REQUIRE(r.has_value());
    CHECK(r->source == ColorSource::Public);
    CHECK(r->color.v == rgb(1, 2, 3).v);

    CHECK_FALSE(b.resolve_color(1, 0).has_value());
}

TEST_CASE("apply(Snapshot) codec 0 rebuilds the canvas and clears the sonar") {
    Board b;
    b.reset(1, 1);
    b.overlay(Sonar{0, 0, {rgb(9, 9, 9)}}, 1);
    REQUIRE(b.sonar_at(0).has_value());

    Snapshot s;
    s.width = 2;
    s.height = 2;
    s.version = 7;
    s.codec = SnapshotCodec::Full;
    s.pixels = {rgb(1, 0, 0), rgb(2, 0, 0), rgb(3, 0, 0), rgb(4, 0, 0)};
    REQUIRE(b.apply(s).has_value());
    CHECK(b.initialized());
    CHECK(b.width() == 2);
    CHECK(b.height() == 2);
    CHECK(b.version() == 7);
    REQUIRE(b.pixels().size() == 4);
    CHECK(b.pixels()[0].v == rgb(1, 0, 0).v);
    CHECK(b.pixels()[3].v == rgb(4, 0, 0).v);
    CHECK(b.color_at(2)->v == rgb(3, 0, 0).v);
    CHECK_FALSE(b.color_at(4).has_value());
    CHECK_FALSE(b.sonar_at(0).has_value());  // the snapshot clears K

    // The sonar table now uses the snapshot dimensions.
    b.overlay(Sonar{3, 0, {rgb(7, 7, 7)}}, 8);
    REQUIRE(b.sonar_at(3).has_value());
    CHECK(b.sonar_at(3)->first.v == rgb(7, 7, 7).v);
    CHECK(b.sonar_at(3)->second == 8);

    // codec=Full must not carry history layers.
    Snapshot with_layers = s;
    with_layers.l1.assign(4, Color{});
    auto r = b.apply(with_layers);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == ErrorKind::InvalidArgument);
    CHECK(b.version() == 7);
    CHECK(b.color_at(0)->v == rgb(1, 0, 0).v);
}

TEST_CASE("apply(Snapshot) codec 1 keeps the history layers") {
    const Color a = rgb(1, 0, 0);
    const Color bcol = rgb(0, 1, 0);
    Snapshot s = make_snapshot(2, 2, 9, SnapshotCodec::FullWithHistory, a);
    s.pixels[0] = bcol;
    s.l1[0] = a;
    s.l2[0] = Color{};  // "empty"; normalized to the current color B

    Board b;
    REQUIRE(b.apply(s).has_value());
    CHECK(b.version() == 9);
    CHECK(b.color_at(0)->v == bcol.v);

    // l1[0] = A, so color code 0 restores A.
    REQUIRE(b.apply(single_segment(9, 10, 1, {code_entry(0, 0)})).has_value());
    CHECK(b.color_at(0)->v == a.v);

    Snapshot bad = s;
    bad.l1.pop_back();
    auto r = b.apply(bad);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == ErrorKind::InvalidArgument);
    CHECK(b.version() == 10);
}

TEST_CASE("apply(Snapshot) rejects malformed payloads") {
    Board b;
    Snapshot good = make_snapshot(2, 2, 1, SnapshotCodec::Full);

    Snapshot bad = good;
    bad.pixels.pop_back();
    CHECK_FALSE(b.apply(bad).has_value());

    bad = good;
    bad.width = 0;
    CHECK_FALSE(b.apply(bad).has_value());

    bad = good;
    bad.codec = static_cast<SnapshotCodec>(7);
    CHECK_FALSE(b.apply(bad).has_value());

    Snapshot hist = make_snapshot(2, 2, 1, SnapshotCodec::FullWithHistory);
    hist.l2.pop_back();
    CHECK_FALSE(b.apply(hist).has_value());

    CHECK_FALSE(b.initialized());
}

TEST_CASE("PUBLIC order 0 keys are pixel indices") {
    Board b;
    REQUIRE(b.apply(make_snapshot(4, 3, 1, SnapshotCodec::Full)).has_value());

    const Color a = rgb(1, 0, 0);
    const Color c = rgb(2, 0, 0);
    const Color d = rgb(3, 0, 0);
    auto pb = single_segment(1, 2, 1, {entry(0, a), entry(5, c), entry(11, d)});
    REQUIRE(b.apply(pb).has_value());
    CHECK(b.version() == 2);

    CHECK(b.color_at(0)->v == a.v);
    CHECK(b.color_at(5)->v == c.v);
    CHECK(b.color_at(11)->v == d.v);
    CHECK(b.color_at(1)->v == kBase.v);
}

TEST_CASE("PUBLIC order 1 keys are column-major") {
    Board b;
    REQUIRE(b.apply(make_snapshot(4, 3, 1, SnapshotCodec::Full)).has_value());

    const Color c0 = rgb(1, 0, 0);
    const Color c5 = rgb(2, 0, 0);
    const Color c7 = rgb(3, 0, 0);
    const Color c10 = rgb(4, 0, 0);
    const Color c11 = rgb(5, 0, 0);
    auto pb = single_segment(
        1, 2, 1,
        {entry(0, c0), entry(5, c5), entry(7, c7), entry(10, c10), entry(11, c11)},
        1);
    REQUIRE(b.apply(pb).has_value());

    // i = (k % H) * W + k / H, W=4, H=3.
    CHECK(b.color_at(0)->v == c0.v);
    CHECK(b.color_at(9)->v == c5.v);
    CHECK(b.color_at(6)->v == c7.v);
    CHECK(b.color_at(7)->v == c10.v);
    CHECK(b.color_at(11)->v == c11.v);
    CHECK(b.color_at(5)->v == kBase.v);  // not touched by this batch
}

TEST_CASE("PUBLIC rejects out-of-range keys") {
    Board b;
    REQUIRE(b.apply(make_snapshot(4, 3, 1, SnapshotCodec::Full)).has_value());

    auto r = b.apply(single_segment(1, 2, 1, {entry(12, rgb(1, 1, 1))}));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == ErrorKind::InvalidArgument);

    auto r1 = b.apply(single_segment(1, 2, 1, {entry(12, rgb(1, 1, 1))}, 1));
    REQUIRE_FALSE(r1.has_value());
    CHECK(r1.error().kind == ErrorKind::InvalidArgument);

    CHECK(b.version() == 1);
    CHECK(b.color_at(0)->v == kBase.v);
}

TEST_CASE("apply(PublicBatch) requires initialization") {
    Board b;
    auto r = b.apply(single_segment(0, 1, 1, {entry(0, rgb(1, 1, 1))}));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == ErrorKind::InvalidArgument);
    CHECK_FALSE(r.error().message.empty());
    CHECK_FALSE(b.color_at(0).has_value());
}

TEST_CASE("PUBLIC color codes follow the cycle from protocol §3.7.3") {
    const Color a = rgb(10, 0, 0);
    const Color bcol = rgb(0, 10, 0);
    const Color ccol = rgb(0, 0, 10);

    Board b;
    REQUIRE(b.apply(one_pixel(1, a)).has_value());

    // A -> B (plain color)
    REQUIRE(b.apply(single_segment(1, 2, 1, {entry(0, bcol)})).has_value());
    CHECK(b.color_at(0)->v == bcol.v);
    CHECK(b.version() == 2);

    // l2 was empty in the snapshot: a code-1 redraw must be a no-op.
    REQUIRE(b.apply(single_segment(2, 3, 1, {code_entry(0, 1)})).has_value());
    CHECK(b.color_at(0)->v == bcol.v);

    // B -> C
    REQUIRE(b.apply(single_segment(3, 4, 1, {entry(0, ccol)})).has_value());
    CHECK(b.color_at(0)->v == ccol.v);

    // code 0 -> l1 = the previous color (B)
    REQUIRE(b.apply(single_segment(4, 5, 1, {code_entry(0, 0)})).has_value());
    CHECK(b.color_at(0)->v == bcol.v);

    // code 1 -> l2 = the color before the previous one (A)
    REQUIRE(b.apply(single_segment(5, 6, 1, {code_entry(0, 1)})).has_value());
    CHECK(b.color_at(0)->v == a.v);
}

TEST_CASE("PUBLIC code 2 is an in-place redraw that leaves history untouched") {
    const Color a = rgb(10, 0, 0);
    const Color bcol = rgb(0, 10, 0);

    Board b;
    REQUIRE(b.apply(one_pixel(1, a)).has_value());
    REQUIRE(b.apply(single_segment(1, 2, 1, {entry(0, bcol)})).has_value());
    CHECK(b.color_at(0)->v == bcol.v);

    REQUIRE(b.apply(single_segment(2, 3, 1, {code_entry(0, 2)})).has_value());
    CHECK(b.color_at(0)->v == bcol.v);

    // If code 2 had rewritten l1/l2 this would not restore A.
    REQUIRE(b.apply(single_segment(3, 4, 1, {code_entry(0, 0)})).has_value());
    CHECK(b.color_at(0)->v == a.v);
}

TEST_CASE("PUBLIC code 0/1 without history layers is rejected") {
    Board b;
    b.reset(2, 2);
    auto r = b.apply(single_segment(0, 1, 1, {code_entry(0, 0)}));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == ErrorKind::InvalidArgument);

    // Plain colors still apply on a reset()-only board.
    const Color a = rgb(1, 2, 3);
    REQUIRE(b.apply(single_segment(0, 1, 1, {entry(0, a)})).has_value());
    CHECK(b.color_at(0)->v == a.v);
    CHECK(b.version() == 1);

    Snapshot full = make_snapshot(1, 1, 2, SnapshotCodec::Full, kBase);
    Board c;
    REQUIRE(c.apply(full).has_value());
    auto r2 = c.apply(single_segment(2, 3, 1, {code_entry(0, 1)}));
    REQUIRE_FALSE(r2.has_value());
    CHECK(r2.error().kind == ErrorKind::InvalidArgument);
}

TEST_CASE("PUBLIC applies segments in order and ends at to") {
    const Color a = rgb(10, 0, 0);
    const Color bcol = rgb(0, 10, 0);
    const Color ccol = rgb(0, 0, 10);

    Board b;
    REQUIRE(b.apply(one_pixel(1, a)).has_value());

    PublicBatch pb;
    pb.from = 10;
    pb.to = 20;
    pb.order = 0;
    PublicSegment s1;
    s1.tick = 3;
    s1.entries = {entry(0, bcol)};
    PublicSegment s2;
    s2.tick = 10;
    s2.entries = {entry(0, ccol)};
    pb.segments = {s1, s2};
    REQUIRE(b.apply(pb).has_value());
    CHECK(b.version() == 20);
    CHECK(b.color_at(0)->v == ccol.v);

    PublicBatch empty;
    empty.from = 20;
    empty.to = 30;
    empty.order = 0;
    REQUIRE(b.apply(empty).has_value());
    CHECK(b.version() == 30);
    CHECK(b.color_at(0)->v == ccol.v);
}

TEST_CASE("PUBLIC validation is atomic") {
    Board b;
    REQUIRE(b.apply(make_snapshot(4, 3, 1, SnapshotCodec::Full)).has_value());

    PublicBatch pb;
    pb.from = 1;
    pb.to = 2;
    pb.order = 0;
    PublicSegment s1;
    s1.tick = 1;
    s1.entries = {entry(0, rgb(1, 1, 1))};
    PublicSegment s2;
    s2.tick = 2;
    s2.entries = {entry(12, rgb(2, 2, 2))};  // out of range
    pb.segments = {s1, s2};

    auto r = b.apply(pb);
    REQUIRE_FALSE(r.has_value());
    CHECK(b.version() == 1);
    CHECK(b.color_at(0)->v == kBase.v);
}

TEST_CASE("PUBLIC rejects malformed batches") {
    Board b;
    REQUIRE(b.apply(make_snapshot(2, 2, 1, SnapshotCodec::Full)).has_value());

    CHECK_FALSE(
        b.apply(single_segment(5, 5, 1, {entry(0, rgb(1, 1, 1))})).has_value());
    CHECK_FALSE(
        b.apply(single_segment(1, 2, 3, {entry(0, rgb(1, 1, 1))})).has_value());
    CHECK_FALSE(
        b.apply(single_segment(1, 2, 1, {entry(0, rgb(1, 1, 1))}, 2)).has_value());
    CHECK_FALSE(
        b.apply(single_segment(1, 2, 1, {entry(0, Color{0x00112233u})})).has_value());

    PublicBatch pb;
    pb.from = 1;
    pb.to = 3;
    pb.order = 0;
    PublicSegment s1;
    s1.tick = 2;
    s1.entries = {entry(0, rgb(1, 1, 1))};
    PublicSegment s2;
    s2.tick = 2;
    s2.entries = {entry(1, rgb(1, 1, 1))};
    pb.segments = {s1, s2};
    CHECK_FALSE(b.apply(pb).has_value());

    CHECK(b.version() == 1);
    CHECK(b.color_at(0)->v == kBase.v);
}

TEST_CASE("Board sonar overlay is exposed separately and pruned by version") {
    Board b;
    b.reset(4, 3);
    REQUIRE(b.color_at(5).has_value());
    CHECK(b.color_at(5)->v == 0);

    const Color c1 = rgb(1, 0, 0);
    const Color c2 = rgb(2, 0, 0);
    const Color c3 = rgb(3, 0, 0);
    const Color c4 = rgb(4, 0, 0);
    Sonar s;
    s.center = 5;
    s.radius = 1;
    s.colors = {Color{}, c1, c2, c3, c4};  // first cell is the out-of-board sentinel
    b.overlay(s, 100);

    CHECK_FALSE(b.sonar_at(1).has_value());
    REQUIRE(b.sonar_at(4).has_value());
    CHECK(b.sonar_at(4)->first.v == c1.v);
    CHECK(b.sonar_at(4)->second == 100);
    REQUIRE(b.sonar_at(5).has_value());
    CHECK(b.sonar_at(5)->first.v == c2.v);
    REQUIRE(b.sonar_at(6).has_value());
    CHECK(b.sonar_at(6)->first.v == c3.v);
    REQUIRE(b.sonar_at(9).has_value());
    CHECK(b.sonar_at(9)->first.v == c4.v);
    CHECK(b.color_at(5)->v == 0);  // the public canvas is not modified

    b.prune(99);
    CHECK(b.sonar_at(4).has_value());
    b.prune(100);
    CHECK_FALSE(b.sonar_at(4).has_value());
    CHECK_FALSE(b.sonar_at(5).has_value());
    CHECK_FALSE(b.sonar_at(6).has_value());
    CHECK_FALSE(b.sonar_at(9).has_value());
}

TEST_CASE("Board prune keeps a newer sonar entry at the same pixel") {
    Board b;
    b.reset(4, 3);
    b.overlay(Sonar{5, 0, {rgb(1, 1, 1)}}, 10);
    b.overlay(Sonar{5, 0, {rgb(2, 2, 2)}}, 20);

    b.prune(10);
    REQUIRE(b.sonar_at(5).has_value());
    CHECK(b.sonar_at(5)->second == 20);
    CHECK(b.sonar_at(5)->first.v == rgb(2, 2, 2).v);

    b.prune(19);
    CHECK(b.sonar_at(5).has_value());
    b.prune(20);
    CHECK_FALSE(b.sonar_at(5).has_value());
}

TEST_CASE("Board overlay skips out-of-board sonar cells") {
    Board b;
    b.reset(4, 3);
    Sonar s;
    s.center = 0;
    s.radius = 1;
    s.colors = {rgb(9, 9, 9), rgb(8, 8, 8), rgb(1, 0, 0), rgb(2, 0, 0),
                rgb(3, 0, 0)};
    b.overlay(s, 50);

    // The two nonzero cells above/left of the canvas must not wrap around.
    REQUIRE(b.sonar_at(0).has_value());
    CHECK(b.sonar_at(0)->first.v == rgb(1, 0, 0).v);
    REQUIRE(b.sonar_at(1).has_value());
    CHECK(b.sonar_at(1)->first.v == rgb(2, 0, 0).v);
    REQUIRE(b.sonar_at(4).has_value());
    CHECK(b.sonar_at(4)->first.v == rgb(3, 0, 0).v);
}

TEST_CASE("Board prune removes only entries up to the public version") {
    Board b;
    b.reset(4, 3);
    b.overlay(Sonar{5, 0, {rgb(1, 0, 0)}}, 10);
    b.overlay(Sonar{9, 0, {rgb(2, 0, 0)}}, 20);

    b.prune(10);
    CHECK_FALSE(b.sonar_at(5).has_value());
    REQUIRE(b.sonar_at(9).has_value());
    CHECK(b.sonar_at(9)->second == 20);

    b.prune(20);
    CHECK_FALSE(b.sonar_at(9).has_value());
}

TEST_CASE("SonarTable add/prune/at") {
    SonarTable t(4, 3);
    Sonar s{5, 1, {Color{}, rgb(1, 0, 0), rgb(2, 0, 0), rgb(3, 0, 0),
                   rgb(4, 0, 0)}};
    t.add(s, 7);

    CHECK_FALSE(t.at(1).has_value());
    REQUIRE(t.at(5).has_value());
    CHECK(t.at(5)->first.v == rgb(2, 0, 0).v);
    CHECK(t.at(5)->second == 7);
    CHECK_FALSE(t.at(12).has_value());

    t.prune(7);
    CHECK_FALSE(t.at(5).has_value());

    // A default-constructed table has no dimensions and ignores add().
    SonarTable none;
    none.add(s, 1);
    CHECK_FALSE(none.at(5).has_value());
}

TEST_CASE("Board is movable") {
    Board a;
    a.reset(2, 2);
    a.overlay(Sonar{0, 0, {rgb(1, 2, 3)}}, 5);

    Board b = std::move(a);
    CHECK(b.initialized());
    CHECK(b.width() == 2);
    REQUIRE(b.sonar_at(0).has_value());
    CHECK(b.sonar_at(0)->second == 5);
}

TEST_CASE("resolve_color makes the sonar/public choice explicit") {
    Board board;
    board.reset(2, 2);
    CHECK_FALSE(board.resolve_color(0, 0).has_value());

    Sonar sonar;
    sonar.center = 0;
    sonar.radius = 0;
    sonar.colors = {Color::rgba(9, 9, 9)};
    board.overlay(sonar, 5);
    auto r = board.resolve_color(0, 5);
    REQUIRE(r.has_value());
    CHECK(r->source == ColorSource::Sonar);
    CHECK(r->color.v == Color::rgba(9, 9, 9).v);
    CHECK(r->tick == 5);

    Snapshot snapshot;
    snapshot.width = 2;
    snapshot.height = 2;
    snapshot.version = 10;
    snapshot.codec = SnapshotCodec::Full;
    snapshot.pixels = {Color::rgba(1, 1, 1), Color::rgba(2, 2, 2),
                       Color::rgba(3, 3, 3), Color::rgba(4, 4, 4)};
    REQUIRE(board.apply(snapshot).has_value());

    // The caller's freshness threshold decides: tick 5 is too old for this
    // reader, so public answers.  This is exactly the guard that prevents a
    // stale sonar from masking a later public attack.
    r = board.resolve_color(0, 11);
    REQUIRE(r.has_value());
    CHECK(r->source == ColorSource::Public);
    CHECK(r->color.v == Color::rgba(1, 1, 1).v);

    board.overlay(sonar, 15);
    r = board.resolve_color(0, 15);
    REQUIRE(r.has_value());
    CHECK(r->source == ColorSource::Sonar);
    CHECK(r->color.v == Color::rgba(9, 9, 9).v);
    board.prune(15);
    r = board.resolve_color(0, 0);
    REQUIRE(r.has_value());
    CHECK(r->source == ColorSource::Public);
    CHECK(r->color.v == Color::rgba(1, 1, 1).v);

    // observe() overlays a whole TICK.
    Tick tick;
    tick.number = 20;
    tick.sonars = {sonar};
    board.observe(tick);
    r = board.resolve_color(0, 20);
    REQUIRE(r.has_value());
    CHECK(r->source == ColorSource::Sonar);

    // Sonar-only pixels are known; untouched pixels are unknown.
    Board overlay_only;
    overlay_only.reset(2, 2);
    overlay_only.overlay(sonar, 3);
    r = overlay_only.resolve_color(0, 0);
    REQUIRE(r.has_value());
    CHECK(r->source == ColorSource::Sonar);
    CHECK_FALSE(overlay_only.resolve_color(1, 0).has_value());
}

