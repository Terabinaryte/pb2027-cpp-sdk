// M0 transport smoke test: TLS WebSocket handshake + binary echo.
// Uses the vendored websocketclient-cpp headers directly (the SDK transport
// adapter arrives in M2). Run against tests/mock_server.py.

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "ws_client/ws_client.hpp"
#include "ws_client/transport/builtin/DnsResolver.hpp"
#include "ws_client/transport/builtin/OpenSslSocket.hpp"
#include "ws_client/transport/builtin/TcpSocket.hpp"

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "localhost";
    const std::string port = argc > 2 ? argv[2] : "9443";

    ws_client::ConsoleLogger logger{ws_client::LogLevel::E};
    auto url = ws_client::URL::parse("wss://" + host + ":" + port + "/");
    if (!url) {
        std::cerr << "URL parse failed\n";
        return 1;
    }

    ws_client::DnsResolver dns(&logger);
    auto addrs = dns.resolve(url->host(), url->port_str(), ws_client::AddrType::ipv4);
    if (!addrs || addrs->empty()) {
        std::cerr << "DNS resolve failed\n";
        return 1;
    }

    ws_client::TcpSocket tcp(&logger, std::move((*addrs)[0]));
    if (!tcp.init()) {
        std::cerr << "socket init failed\n";
        return 1;
    }

    ws_client::OpenSslContext ctx(&logger);
    if (!ctx.init()) {
        std::cerr << "TLS context init failed\n";
        return 1;
    }
    // Self-signed test certificate: verification is intentionally disabled here.
    auto ssl = ws_client::OpenSslSocket(
        &logger, std::move(tcp), &ctx, url->host(), false);
    if (!ssl.init()) {
        std::cerr << "TLS socket init failed\n";
        return 1;
    }
    if (!ssl.connect(5s)) {
        std::cerr << "TLS connect failed\n";
        return 1;
    }

    auto client = ws_client::WebSocketClient(&logger, std::move(ssl));
    auto handshake = ws_client::Handshake(&logger, *url);
    if (!client.handshake(handshake, 5s)) {
        std::cerr << "WS handshake failed\n";
        return 1;
    }

    auto buffer = ws_client::Buffer::create(4096, 1u << 20);
    if (!buffer) {
        std::cerr << "buffer alloc failed\n";
        return 1;
    }

    const std::string payload = "pb2027-smoke";
    const std::string expected = payload;  // send_message masks the buffer in place
    ws_client::Message msg(ws_client::MessageType::binary, payload);
    if (!client.send_message(msg)) {
        std::cerr << "send failed\n";
        return 1;
    }

    auto var = client.read_message(*buffer, 5s);
    if (auto* echo = std::get_if<ws_client::Message>(&var)) {
        const auto data = echo->data;
        if (data.size() != expected.size() ||
            std::memcmp(data.data(), expected.data(), expected.size()) != 0) {
            std::cerr << "echo mismatch\n";
        }
        std::cout << "OK echo " << data.size() << " bytes\n";
    } else if (auto* err = std::get_if<ws_client::WSError>(&var)) {
        std::cerr << "read failed: " << err->message << "\n";
        return 1;
    } else {
        std::cerr << "unexpected frame\n";
        return 1;
    }

    (void)client.close(ws_client::close_code::normal_closure);
    return 0;
}
