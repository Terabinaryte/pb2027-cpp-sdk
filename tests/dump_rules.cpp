// Diagnostic: print the first compressed-downlink payload from a real /ws.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <zstd.h>

#include "ws_client/transport/builtin/DnsResolver.hpp"
#include "ws_client/transport/builtin/OpenSslSocket.hpp"
#include "ws_client/transport/builtin/TcpSocket.hpp"
#include "ws_client/ws_client.hpp"

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "pb.ika.zip";
    const std::string port = argc > 2 ? argv[2] : "443";
    const std::string query = argc > 3 ? argv[3] : "public=0";

    ws_client::ConsoleLogger logger{ws_client::LogLevel::E};
    auto url = ws_client::URL::parse("wss://" + host + ":" + port + "/ws?" + query);
    if (!url) return 1;

    ws_client::DnsResolver<ws_client::ConsoleLogger> dns(&logger);
    auto addrs = dns.resolve(url->host(), url->port_str(), ws_client::AddrType::ipv4);
    if (!addrs || addrs->empty()) return 1;

    ws_client::TcpSocket<ws_client::ConsoleLogger> tcp(&logger, std::move((*addrs)[0]));
    if (!tcp.init()) return 1;

    ws_client::OpenSslContext<ws_client::ConsoleLogger> ctx(&logger);
    if (!ctx.init()) return 1;
    if (!ctx.set_default_verify_paths()) return 1;

    ws_client::OpenSslSocket<ws_client::ConsoleLogger> ssl(
        &logger, std::move(tcp), &ctx, url->host(), true);
    if (!ssl.init()) return 1;
    if (!ssl.connect(5s)) return 1;

    auto client = ws_client::WebSocketClient(&logger, std::move(ssl));
    auto hs = ws_client::Handshake(&logger, *url);
    if (!client.handshake(hs, 5s)) return 1;

    auto buffer = ws_client::Buffer::create(64 * 1024, 16u << 20);
    if (!buffer) return 1;

    auto var = client.read_message(*buffer, 5s);
    auto* msg = std::get_if<ws_client::Message>(&var);
    if (!msg) {
        std::cerr << "no message\n";
        return 1;
    }

    std::vector<std::byte> out(16u << 20);
    const std::size_t n = ZSTD_decompress(out.data(), out.size(), msg->data.data(),
                                         msg->data.size());
    if (ZSTD_isError(n)) {
        std::cerr << "zstd: " << ZSTD_getErrorName(n) << "\n";
        return 1;
    }
    out.resize(n);
    std::cout << "size=" << n << " type=" << std::to_integer<int>(out[0]) << "\n";
    for (std::size_t i = 0; i < n; ++i) {
        std::printf("%02x ", std::to_integer<unsigned>(out[i]));
        if (i % 16 == 15) std::printf("\n");
    }
    std::printf("\n");
    return 0;
}
