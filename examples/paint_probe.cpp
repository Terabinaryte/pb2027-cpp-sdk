// Real-server paint probe (test environment only).
//
// Requests `count` tokens for the given uid range, then:
//   1. binds all tokens;
//   2. paints the current color on a bottom-row pixel per slot (no visual change);
//   3. paints the same pixel again to observe COOLING;
//   4. reconnects and paints once more.
//
// Usage: pb2027_paint_probe [host] [count] [uid-base]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "pb2027/api.hpp"
#include "pb2027/board.hpp"
#include "pb2027/client.hpp"

using namespace pb2027;
using namespace std::chrono_literals;

namespace {

std::string mask(const PaintToken& token) {
    std::string uuid = token.to_uuid();
    return uuid.substr(0, 8) + "...";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "pb.ika.zip";
    const int count = argc > 2 ? std::stoi(argv[2]) : 5;
    const Uid uid_base =
        argc > 3 ? static_cast<Uid>(std::stoul(argv[3])) : 910001u;
    if (count <= 0 || count > 512) {
        std::cerr << "count must be 1..512\n";
        return 2;
    }

    api::Endpoint ep;
    ep.host = host;
    ep.port = 443;
    ep.tls = true;
    ep.verify_peer = true;

    auto meta = api::fetch_meta(ep);
    if (!meta) {
        std::cerr << "meta failed: " << to_string(meta.error()) << "\n";
        return 1;
    }
    if (meta->access_mode != "none") {
        std::cerr << "server is not in test mode (access_mode="
                  << meta->access_mode << ")\n";
        return 1;
    }
    std::cout << "meta: board " << meta->width << "x" << meta->height
              << " max_slots=" << meta->rules.max_slots << "\n";

    auto board_response = api::fetch_board(ep, std::nullopt);
    if (!board_response || !board_response->snapshot) {
        std::cerr << "board fetch failed\n";
        return 1;
    }
    Board board;
    if (auto r = board.apply(*board_response->snapshot); !r) {
        std::cerr << "board apply failed: " << to_string(r.error()) << "\n";
        return 1;
    }

    std::vector<api::IssuedToken> issued(static_cast<std::size_t>(count));
    constexpr int kTokenBatch = 16;
    for (int start = 0; start < count; start += kTokenBatch) {
        const int end = std::min(count, start + kTokenBatch);
        std::vector<std::future<std::expected<api::IssuedToken, Error>>> futures;
        futures.reserve(static_cast<std::size_t>(end - start));
        for (int i = start; i < end; ++i) {
            futures.push_back(std::async(std::launch::async, [&ep, uid_base, i] {
                return api::acquire_token(
                    ep, api::UidRequest{static_cast<Uid>(uid_base + i)});
            }));
        }
        for (int i = start; i < end; ++i) {
            auto token = futures[static_cast<std::size_t>(i - start)].get();
            if (!token) {
                std::cerr << "token " << i << " failed: "
                          << to_string(token.error()) << "\n";
                return 1;
            }
            issued[static_cast<std::size_t>(i)] = std::move(*token);
        }
    }
    std::cout << "tokens: " << issued.size() << " (uid "
              << uid_base << ".." << uid_base + count - 1 << ")\n";

    ClientConfig cfg;
    cfg.host = host;
    cfg.port = 443;
    cfg.tls = true;
    cfg.verify_peer = true;
    cfg.public_stream = false;
    cfg.connect_timeout = 10s;
    cfg.read_timeout = 10s;

    Client client(cfg);
    std::uint64_t results = 0;
    std::vector<Result> last_results;
    std::vector<Sonar> last_sonars;
    client.on_tick([&](const Tick& t) {
        results += t.results.size();
        last_results = t.results;
        last_sonars = t.sonars;
    });
    client.on_error([](const Error& e) {
        std::cerr << "client error: " << to_string(e) << "\n";
    });
    client.on_close([](const CloseInfo& c) {
        std::cerr << "client closed: " << c.code << " " << c.reason << "\n";
    });

    auto connected = client.connect();
    if (!connected) {
        std::cerr << "connect failed: " << to_string(connected.error()) << "\n";
        return 1;
    }
    std::cout << "connected\n";

    auto wait_results = [&](std::uint64_t want) {
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (results < want && std::chrono::steady_clock::now() < deadline) {
            if (client.poll(50ms) == PollStatus::Closed) {
                break;
            }
        }
        return results >= want;
    };

    for (int i = 0; i < count; ++i) {
        client.bind(static_cast<SlotId>(i), issued[static_cast<std::size_t>(i)].token);
    }
    auto bind_flush = client.flush();
    if (!bind_flush.sent || !wait_results(static_cast<std::uint64_t>(count))) {
        std::cerr << "bind failed\n";
        return 1;
    }
    for (const auto& r : last_results) {
        if (r.code != ResultCode::Ok) {
            std::cerr << "bind result not OK: " << static_cast<int>(r.code) << "\n";
            return 1;
        }
    }
    std::cout << "bound " << count << " slots\n";

    // Bottom row, one pixel per slot. Repaint the current color so the canvas
    // does not change visually.
    std::vector<PixelIndex> targets;
    std::vector<Color> colors;
    const std::uint32_t y = 599;
    for (int i = 0; i < count; ++i) {
        const PixelIndex idx = y * board.width() + static_cast<std::uint32_t>(i);
        targets.push_back(idx);
        colors.push_back(board.color_at(idx).value_or(Color::rgba(0, 0, 0)));
    }

    const std::uint64_t before_paints = results;
    for (int i = 0; i < count; ++i) {
        client.paint(static_cast<SlotId>(i), targets[static_cast<std::size_t>(i)],
                     colors[static_cast<std::size_t>(i)]);
    }
    if (!client.flush().sent ||
        !wait_results(before_paints + static_cast<std::uint64_t>(count))) {
        std::cerr << "paint failed\n";
        return 1;
    }
    int ok = 0;
    for (const auto& r : last_results) {
        if (r.code == ResultCode::Ok) {
            ++ok;
        } else {
            std::cerr << "paint result " << static_cast<int>(r.code) << "\n";
        }
    }
    std::cout << "painted " << ok << "/" << count << " OK, sonars="
              << last_sonars.size() << "\n";

    // Immediate repaint on slot 0 must be COOLING.
    const std::uint64_t before_cooling = results;
    client.paint(0, targets[0], colors[0]);
    if (!client.flush().sent || !wait_results(before_cooling + 1)) {
        std::cerr << "cooling probe failed\n";
        return 1;
    }
    bool saw_cooling = false;
    for (const auto& r : last_results) {
        if (r.code == ResultCode::Cooling) {
            saw_cooling = true;
            std::cout << "cooling after " << r.value << " us\n";
        }
    }
    if (!saw_cooling) {
        std::cerr << "expected COOLING, got something else\n";
        return 1;
    }

    // Reconnect: desired slots are rebound automatically.
    if (auto r = client.reconnect(); !r) {
        std::cerr << "reconnect failed: " << to_string(r.error()) << "\n";
        return 1;
    }
    const std::uint64_t before_rebind = results;
    if (!wait_results(before_rebind + static_cast<std::uint64_t>(count))) {
        std::cerr << "rebind failed\n";
        return 1;
    }
    std::cout << "reconnected and rebound " << count << " slots\n";

    std::this_thread::sleep_for(1200ms);  // c0 = 1s, k = 0.1s/px
    const std::uint64_t before_after = results;
    client.paint(0, targets[0], colors[0]);
    if (!client.flush().sent || !wait_results(before_after + 1)) {
        std::cerr << "post-reconnect paint failed\n";
        return 1;
    }
    if (!last_results.empty() && last_results[0].code == ResultCode::Ok) {
        std::cout << "post-reconnect paint OK\n";
    } else {
        std::cerr << "post-reconnect paint not OK\n";
        return 1;
    }

    client.close();
    std::cout << "done\n";
    return 0;
}
