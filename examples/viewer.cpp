// Read-only viewer demo: applies real SNAPSHOT/PUBLIC to a Board.
#include <chrono>
#include <iostream>

#include "pb2027/board.hpp"
#include "pb2027/client.hpp"

using namespace pb2027;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    ClientConfig cfg;
    cfg.host = argc > 1 ? argv[1] : "pb.ika.zip";
    cfg.port = static_cast<std::uint16_t>(argc > 2 ? std::stoi(argv[2]) : 443);
    cfg.tls = true;
    cfg.verify_peer = true;
    cfg.public_stream = true;
    cfg.connect_timeout = 10s;
    cfg.read_timeout = 10s;

    Board board;
    Client client(cfg);

    client.on_snapshot([&](const Snapshot& s) {
        auto r = board.apply(s);
        if (!r) {
            std::cerr << "SNAPSHOT apply failed: " << to_string(r.error()) << "\n";
            return;
        }
        std::cout << "SNAPSHOT applied v=" << board.version() << " "
                  << board.width() << "x" << board.height() << "\n";
    });
    client.on_public([&](const PublicBatch& p) {
        auto r = board.apply(p);
        if (!r) {
            std::cerr << "PUBLIC apply failed: " << to_string(r.error()) << "\n";
            return;
        }
        std::cout << "PUBLIC applied v=" << board.version()
                  << " pixels=" << board.pixels().size() << "\n";
    });
    client.on_close([](const CloseInfo& c) {
        std::cout << "CLOSE " << c.code << " " << c.reason << "\n";
    });
    client.on_error([](const Error& e) {
        std::cerr << "ERROR " << to_string(e) << "\n";
    });

    auto connected = client.connect();
    if (!connected) {
        std::cerr << "connect failed: " << to_string(connected.error()) << "\n";
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           client.state() == State::Open) {
        (void)client.poll(200ms);
    }

    if (board.initialized()) {
        if (auto c = board.color_at(0)) {
            std::cout << "pixel 0 color=" << std::hex << c->v << std::dec << "\n";
        }
    }
    client.close();
    return 0;
}
