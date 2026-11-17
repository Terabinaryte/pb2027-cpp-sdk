#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "pb2027/client.hpp"

using namespace pb2027;
using namespace std::chrono_literals;

namespace {

std::uint16_t test_port() {
    const char* env = std::getenv("PB2027_TEST_PORT");
    return env ? static_cast<std::uint16_t>(std::stoi(env)) : 9444;
}

}  // namespace

TEST_CASE("M3 lost.slot and ambiguous disconnect") {
    ClientConfig cfg;
    cfg.host = "localhost";
    cfg.port = test_port();
    cfg.verify_peer = false;
    cfg.connect_timeout = 5s;
    cfg.read_timeout = 2s;

    Client client(cfg);
    std::vector<Tick> ticks;
    std::vector<CloseInfo> closes;
    client.on_tick([&](const Tick& t) { ticks.push_back(t); });
    client.on_close([&](const CloseInfo& c) { closes.push_back(c); });

    auto connected = client.connect();
    REQUIRE(connected.has_value());
    auto token = PaintToken::from_uuid("00010203-0405-0607-0809-0a0b0c0d0e0f");
    REQUIRE(token.has_value());

    auto pump = [&](std::size_t want) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (ticks.size() < want && std::chrono::steady_clock::now() < deadline) {
            if (client.poll(50ms) == PollStatus::Closed) break;
        }
    };

    client.bind(0, *token);
    CHECK(client.flush().sent);
    pump(ticks.size() + 1);
    REQUIRE(client.active_token(0) == token);

    // Server reports paint OK but also lists slot 0 in lost.slot.
    client.paint(0, 666, Color::rgba(1, 1, 1));
    CHECK(client.flush().sent);
    const std::size_t before_lost = ticks.size();
    pump(before_lost + 1);
    CHECK_FALSE(client.active_token(0).has_value());

    // Rebind, then server drops the TCP connection before replying to the paint.
    client.bind(0, *token);
    CHECK(client.flush().sent);
    const std::size_t before_rebind = ticks.size();
    pump(before_rebind + 1);
    REQUIRE(client.active_token(0) == token);

    client.paint(0, 999, Color::rgba(2, 2, 2));
    CHECK(client.flush().sent);
    for (int i = 0; i < 60 && client.state() != State::Closed; ++i) {
        (void)client.poll(50ms);
    }
    CHECK(client.state() == State::Closed);
    REQUIRE(!closes.empty());
    CHECK(closes.back().ambiguous >= 1);
}

TEST_CASE("M2 client: connect, bind, paint, cooling, submit") {
    ClientConfig cfg;
    cfg.host = "localhost";
    cfg.port = test_port();
    cfg.tls = true;
    cfg.verify_peer = false;
    cfg.public_stream = false;
    cfg.connect_timeout = 5s;
    cfg.read_timeout = 2s;

    std::vector<Rules> rules_seen;
    std::vector<Tick> ticks;
    std::vector<CloseInfo> closes;
    std::vector<Error> errors;

    Client client(cfg);
    client.on_rules([&](const Rules& r) { rules_seen.push_back(r); });
    client.on_tick([&](const Tick& t) { ticks.push_back(t); });
    client.on_close([&](const CloseInfo& c) { closes.push_back(c); });
    client.on_error([&](const Error& e) { errors.push_back(e); });

    auto connected = client.connect();
    REQUIRE(connected.has_value());
    REQUIRE(client.state() == State::Open);
    REQUIRE(!rules_seen.empty());
    CHECK(rules_seen.back().width == 1000);
    CHECK(rules_seen.back().height == 600);
    CHECK(rules_seen.back().max_slots == 8000);
    CHECK(rules_seen.back().cooldown_base_us == 1'000'000);
    CHECK(rules_seen.back().cooldown_per_px_us == 100'000);

    auto pump = [&](std::size_t want) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (ticks.size() < want && std::chrono::steady_clock::now() < deadline) {
            if (client.poll(50ms) == PollStatus::Closed) {
                break;
            }
        }
    };

    auto token = PaintToken::from_uuid("00010203-0405-0607-0809-0a0b0c0d0e0f");
    REQUIRE(token.has_value());

    client.bind(0, *token);
    client.paint(0, 500, Color::rgba(1, 2, 3));
    auto first_flush = client.flush();
    CHECK(first_flush.sent);
    CHECK(first_flush.records == 2);

    pump(1);
    REQUIRE(!ticks.empty());
    const Tick& t = ticks.back();
    REQUIRE(t.results.size() == 2);
    CHECK(t.results[0].kind == RecordKind::Bind);
    CHECK(t.results[0].slot == 0);
    CHECK(t.results[0].code == ResultCode::Ok);
    CHECK(t.results[1].kind == RecordKind::Paint);
    CHECK(t.results[1].code == ResultCode::Ok);

    CHECK(client.active_token(0) == token);
    auto brush = client.brush(0);
    REQUIRE(brush.has_value());
    REQUIRE(brush->idx.has_value());
    CHECK(*brush->idx == 500);

    auto ready = client.ready_at_us(0, 501);
    REQUIRE(ready.has_value());
    CHECK(*ready >= brush->t_us + 1'000'000 + 100'000);

    // Cooling path: the scripted server rejects pixel 777 with value 123456.
    const std::size_t before_cooling = ticks.size();
    client.paint(0, 777, Color::rgba(9, 9, 9));
    CHECK(client.flush().sent);
    pump(before_cooling + 1);
    REQUIRE(ticks.size() > before_cooling);
    const Tick& cooling_tick = ticks.back();
    REQUIRE(cooling_tick.results.size() == 1);
    CHECK(cooling_tick.results[0].kind == RecordKind::Paint);
    CHECK(cooling_tick.results[0].code == ResultCode::Cooling);
    CHECK(cooling_tick.results[0].value == 123456);

    // Explicit submit() keeps its SubmissionId in the result.
    Batch batch;
    batch.paint(0, 600, Color::rgba(4, 5, 6));
    auto submission = client.submit(std::move(batch));
    REQUIRE(submission.has_value());
    CHECK(client.flush().sent);
    pump(ticks.size() + 1);
    bool found = false;
    for (const auto& tick : ticks) {
        for (const auto& result : tick.results) {
            if (result.submission.value == submission->value &&
                result.record == 0 && result.kind == RecordKind::Paint) {
                found = true;
            }
        }
    }
    CHECK(found);

    // Reconnect while Open: connect + rebind desired slots + resume since.
    auto reconnected = client.reconnect();
    REQUIRE(reconnected.has_value());
    CHECK(client.state() == State::Open);
    const std::size_t before_rebind = ticks.size();
    pump(before_rebind + 1);
    CHECK(ticks.size() > before_rebind);
    CHECK(client.active_token(0) == token);

    // Server-initiated close of 4000 must surface as non-retryable.
    client.paint(0, 888, Color::rgba(7, 7, 7));
    CHECK(client.flush().sent);
    for (int i = 0; i < 60 && client.state() != State::Closed; ++i) {
        (void)client.poll(50ms);
    }
    CHECK(client.state() == State::Closed);
    REQUIRE(!closes.empty());
    CHECK(closes.back().code == 4000);
    CHECK_FALSE(closes.back().retryable);

    client.close();
    CHECK(client.state() == State::Closed);
    // The server may have closed first; either way the client must not have
    // surfaced a protocol error.
    for (const auto& error : errors) {
        CHECK(error.kind != ErrorKind::Protocol);
    }
}
