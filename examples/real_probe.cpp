// Read-only real-server probe: connect to /ws?public=1, print RULES/SNAPSHOT.
#include <chrono>
#include <iostream>

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

    Client client(cfg);
    client.on_rules([](const Rules& r) {
        std::cout << "RULES " << r.width << "x" << r.height << " tps=" << r.tps
                  << " slots=" << r.max_slots << " c0=" << r.cooldown_base_us
                  << " k=" << r.cooldown_per_px_us << "\n";
    });
    client.on_snapshot([](const Snapshot& s) {
        std::cout << "SNAPSHOT " << s.width << "x" << s.height << " v=" << s.version
                  << " codec=" << static_cast<int>(s.codec)
                  << " pixels=" << s.pixels.size() << " l1=" << s.l1.size() << "\n";
    });
    client.on_public([](const PublicBatch& p) {
        std::cout << "PUBLIC (" << p.from << "," << p.to << "] segs="
                  << p.segments.size() << " order=" << p.order << "\n";
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
    std::cout << "CONNECTED\n";

    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline &&
           client.state() == State::Open) {
        (void)client.poll(200ms);
    }
    client.close();
    return 0;
}
