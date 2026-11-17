// Paint a solid rectangle on the test server using many tokens.
//
// Each token owns one contiguous horizontal run inside a row, so every
// consecutive stroke is Manhattan distance 1 (c0 + k*1 cooldown).
//
// Usage: pb2027_paint_rect [host] [x] [y] [w] [h] [uid-base] [tokens]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "pb2027/api.hpp"
#include "pb2027/client.hpp"

using namespace pb2027;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "pb.ika.zip";
    const int x0 = argc > 2 ? std::stoi(argv[2]) : 0;
    const int y0 = argc > 3 ? std::stoi(argv[3]) : 0;
    const int width = argc > 4 ? std::stoi(argv[4]) : 50;
    const int height = argc > 5 ? std::stoi(argv[5]) : 50;
    const Uid uid_base =
        argc > 6 ? static_cast<Uid>(std::stoul(argv[6])) : 950001u;
    const int token_count = argc > 7 ? std::stoi(argv[7]) : 300;

    if (width <= 0 || height <= 0 || x0 < 0 || y0 < 0 ||
        x0 + width > 1000 || y0 + height > 600 || token_count <= 0 ||
        token_count > 512) {
        std::cerr << "invalid rectangle/token count\n";
        return 2;
    }

    api::Endpoint ep;
    ep.host = host;
    ep.port = 443;
    ep.tls = true;
    ep.verify_peer = true;

    auto meta = api::fetch_meta(ep);
    if (!meta || meta->access_mode != "none") {
        std::cerr << "test-mode server required\n";
        return 1;
    }
    const std::uint32_t board_w = meta->width;
    const std::uint32_t board_h = meta->height;

    std::vector<api::IssuedToken> issued(static_cast<std::size_t>(token_count));
    constexpr int kTokenBatch = 16;
    for (int start = 0; start < token_count; start += kTokenBatch) {
        const int end = std::min(token_count, start + kTokenBatch);
        std::vector<std::future<std::expected<api::IssuedToken, Error>>> futures;
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
    std::cout << "tokens ready: " << token_count << "\n";

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
    client.on_tick([&](const Tick& t) {
        results += t.results.size();
        last_results = t.results;
    });
    client.on_error([](const Error& e) {
        std::cerr << "client error: " << to_string(e) << "\n";
    });

    auto connected = client.connect();
    if (!connected) {
        std::cerr << "connect failed: " << to_string(connected.error()) << "\n";
        return 1;
    }

    auto wait_results = [&](std::uint64_t want) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (results < want && std::chrono::steady_clock::now() < deadline) {
            if (client.poll(50ms) == PollStatus::Closed) {
                break;
            }
        }
        return results >= want;
    };

    for (int i = 0; i < token_count; ++i) {
        client.bind(static_cast<SlotId>(i), issued[static_cast<std::size_t>(i)].token);
    }
    if (!client.flush().sent ||
        !wait_results(static_cast<std::uint64_t>(token_count))) {
        std::cerr << "bind failed\n";
        return 1;
    }
    std::cout << "bound " << token_count << " slots\n";

    // Split each row into contiguous runs owned by one token.
    const int per_row = std::max(1, token_count / height);
    std::vector<std::vector<PixelIndex>> chunks(static_cast<std::size_t>(token_count));
    for (int row = 0; row < height; ++row) {
        for (int k = 0; k < per_row; ++k) {
            const int slot = row * per_row + k;
            if (slot >= token_count) {
                break;
            }
            const int xs = x0 + (k * width) / per_row;
            const int xe = x0 + ((k + 1) * width) / per_row;
            for (int x = xs; x < xe; ++x) {
                chunks[static_cast<std::size_t>(slot)].push_back(
                    static_cast<PixelIndex>((y0 + row) * static_cast<int>(board_w) + x));
            }
        }
    }

    std::size_t max_chunk = 0;
    for (const auto& c : chunks) {
        max_chunk = std::max(max_chunk, c.size());
    }

    const Color white{0xFFFFFFFFu};
    std::uint64_t painted = 0;
    for (std::size_t step = 0; step < max_chunk; ++step) {
        std::uint64_t before = results;
        int sent = 0;
        for (int slot = 0; slot < token_count; ++slot) {
            const auto& chunk = chunks[static_cast<std::size_t>(slot)];
            if (step < chunk.size()) {
                client.paint(static_cast<SlotId>(slot), chunk[step], white);
                ++sent;
            }
        }
        if (sent == 0) {
            continue;
        }
        if (!client.flush().sent ||
            !wait_results(before + static_cast<std::uint64_t>(sent))) {
            std::cerr << "paint step " << step << " failed\n";
            return 1;
        }
        for (const auto& r : last_results) {
            if (r.code != ResultCode::Ok) {
                std::cerr << "step " << step << " result "
                          << static_cast<int>(r.code) << " value " << r.value
                          << "\n";
                return 1;
            }
        }
        painted += static_cast<std::uint64_t>(sent);
        std::cout << "wave " << step + 1 << "/" << max_chunk << ": " << sent
                  << " pixels\n";
        std::this_thread::sleep_for(1200ms);
    }

    std::cout << "painted " << painted << " pixels "
              << "at " << x0 << "," << y0 << " " << width << "x" << height
              << " (board " << board_w << "x" << board_h << ")\n";
    client.close();
    return 0;
}
