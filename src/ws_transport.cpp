#include "transport.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ws_client/ws_client.hpp"
#include "ws_client/transport/builtin/DnsResolver.hpp"
#include "ws_client/transport/builtin/OpenSslContext.hpp"
#include "ws_client/transport/builtin/OpenSslSocket.hpp"
#include "ws_client/transport/builtin/TcpSocket.hpp"

namespace pb2027::detail {
namespace {

using namespace std::chrono_literals;
using Logger = ws_client::ConsoleLogger;
using SslSocket = ws_client::OpenSslSocket<Logger>;
using WsClient = ws_client::WebSocketClient<Logger, SslSocket>;

Error map_error(const ws_client::WSError& e, ErrorKind fallback = ErrorKind::Transport) {
    Error out;
    out.kind = fallback;
    if (e.code == ws_client::WSErrorCode::timeout_error) {
        out.kind = ErrorKind::Timeout;
    } else if (e.code == ws_client::WSErrorCode::connection_closed) {
        out.kind = ErrorKind::Closed;
    } else if (e.code == ws_client::WSErrorCode::protocol_error) {
        out.kind = ErrorKind::Protocol;
    } else if (e.code == ws_client::WSErrorCode::url_error) {
        out.kind = ErrorKind::InvalidArgument;
    }
    out.message = e.message;
    if (e.close_with_code != ws_client::close_code::not_set) {
        out.close_code = static_cast<std::uint16_t>(e.close_with_code);
    }
    return out;
}

class WsTransport final : public Transport {
public:
    WsTransport() : logger_(ws_client::LogLevel::E) {}
    ~WsTransport() override { close(); }

    std::expected<void, Error> connect(
        const ClientConfig& cfg,
        std::optional<TickNumber> since,
        std::chrono::milliseconds timeout) override {
        if (!cfg.tls) {
            return std::unexpected(Error{ErrorKind::Unsupported,
                                         "M2 transport only supports wss://",
                                         std::nullopt, std::nullopt});
        }
        if (cfg.proxy.has_value()) {
            return std::unexpected(Error{ErrorKind::Unsupported,
                                         "proxy transport is not implemented yet",
                                         std::nullopt, std::nullopt});
        }

        std::string url_text = "wss://" + cfg.host + ":" + std::to_string(cfg.port) +
                               "/ws?public=" + (cfg.public_stream ? "1" : "0");
        if (since.has_value()) {
            url_text += "&since=" + std::to_string(*since);
        }

        auto url = ws_client::URL::parse(url_text);
        if (!url) {
            return std::unexpected(map_error(url.error(), ErrorKind::InvalidArgument));
        }

        ws_client::DnsResolver<Logger> dns(&logger_);
        auto addresses = dns.resolve(url->host(), url->port_str(), ws_client::AddrType::ipv4);
        if (!addresses || addresses->empty()) {
            Error e;
            e.kind = ErrorKind::Transport;
            e.message = addresses ? "no IPv4 address for host" : addresses.error().message;
            return std::unexpected(std::move(e));
        }

        ws_client::TcpSocket<Logger> tcp(&logger_, std::move((*addresses)[0]));
        if (auto r = tcp.init(); !r) {
            return std::unexpected(map_error(r.error()));
        }

        context_ = std::make_unique<ws_client::OpenSslContext<Logger>>(&logger_);
        if (auto r = context_->init(); !r) {
            return std::unexpected(map_error(r.error()));
        }
        if (cfg.verify_peer) {
            if (auto r = context_->set_default_verify_paths(); !r) {
                return std::unexpected(map_error(r.error()));
            }
        }
        if (!cfg.ca_file.empty()) {
            if (auto r = context_->load_verify_file(cfg.ca_file); !r) {
                return std::unexpected(map_error(r.error()));
            }
        }

        SslSocket ssl(&logger_, std::move(tcp), context_.get(), url->host(),
                      cfg.verify_peer);
        if (auto r = ssl.init(); !r) {
            return std::unexpected(map_error(r.error(), ErrorKind::Tls));
        }
        if (auto r = ssl.connect(timeout); !r) {
            return std::unexpected(map_error(r.error(), ErrorKind::Tls));
        }

        client_ = std::make_unique<WsClient>(&logger_, std::move(ssl));
        auto handshake = ws_client::Handshake(&logger_, *url);
        if (auto r = client_->handshake(handshake, timeout); !r) {
            return std::unexpected(map_error(r.error(), ErrorKind::Handshake));
        }

        auto buffer = ws_client::Buffer::create(64u * 1024u, 16u << 20);
        if (!buffer) {
            return std::unexpected(map_error(buffer.error()));
        }
        buffer_ = std::move(*buffer);
        open_ = true;
        return {};
    }

    std::expected<bool, Error> wait_readable(std::chrono::milliseconds timeout) override {
        if (!client_ || !open_) {
            return std::unexpected(Error{ErrorKind::Closed, "connection is closed",
                                         std::nullopt, std::nullopt});
        }
        if (timeout <= std::chrono::milliseconds::zero()) {
            return false;  // websocketclient asserts on non-positive timeouts
        }
        auto r = client_->wait_message(timeout);
        if (!r) {
            return std::unexpected(map_error(r.error()));
        }
        return *r;
    }

    std::expected<Inbound, Error> read(std::chrono::milliseconds timeout) override {
        if (!client_ || !open_) {
            return std::unexpected(Error{ErrorKind::Closed, "connection is closed",
                                         std::nullopt, std::nullopt});
        }
        auto var = client_->read_message(*buffer_, timeout);
        if (auto* msg = std::get_if<ws_client::Message>(&var)) {
            Inbound in;
            in.kind = Inbound::Kind::Binary;
            in.data.assign(msg->data.begin(), msg->data.end());
            return in;
        }
        if (auto* ping = std::get_if<ws_client::PingFrame>(&var)) {
            Inbound in;
            in.kind = Inbound::Kind::Ping;
            const auto payload = ping->payload_bytes();
            in.data.assign(payload.begin(), payload.end());
            return in;
        }
        if (std::get_if<ws_client::PongFrame>(&var)) {
            Inbound in;
            in.kind = Inbound::Kind::Pong;
            return in;
        }
        if (auto* close = std::get_if<ws_client::CloseFrame>(&var)) {
            Inbound in;
            in.kind = Inbound::Kind::Close;
            in.close_code = static_cast<std::uint16_t>(close->get_close_code());
            in.close_reason = std::string(close->get_reason());
            open_ = false;
            return in;
        }
        if (auto* err = std::get_if<ws_client::WSError>(&var)) {
            return std::unexpected(map_error(*err));
        }
        return std::unexpected(Error{ErrorKind::Internal, "unexpected frame type",
                                     std::nullopt, std::nullopt});
    }

    std::expected<void, Error> send(std::span<const std::byte> payload) override {
        if (!client_ || !open_) {
            return std::unexpected(Error{ErrorKind::Closed, "connection is closed",
                                         std::nullopt, std::nullopt});
        }
        // send_message masks the buffer in place, so hand it a private copy.
        std::vector<std::byte> copy(payload.begin(), payload.end());
        ws_client::Message msg(ws_client::MessageType::binary,
                               std::span<const std::byte>(copy));
        auto r = client_->send_message(msg);
        if (!r) {
            return std::unexpected(map_error(r.error()));
        }
        return {};
    }

    std::expected<void, Error> send_pong(std::span<const std::byte> payload) override {
        if (!client_ || !open_) {
            return std::unexpected(Error{ErrorKind::Closed, "connection is closed",
                                         std::nullopt, std::nullopt});
        }
        std::vector<std::byte> copy(payload.begin(), payload.end());
        auto r = client_->send_pong_frame(std::span<std::byte>(copy));
        if (!r) {
            return std::unexpected(map_error(r.error()));
        }
        return {};
    }

    void close() noexcept override {
        if (client_ && open_) {
            (void)client_->close(ws_client::close_code::normal_closure, 1s);
        }
        open_ = false;
        client_.reset();
        buffer_ = std::nullopt;
        context_.reset();
    }

    bool is_open() const noexcept override { return open_ && client_ && client_->is_open(); }

private:
    Logger logger_;
    std::unique_ptr<ws_client::OpenSslContext<Logger>> context_;
    std::unique_ptr<WsClient> client_;
    std::optional<ws_client::Buffer> buffer_;
    bool open_ = false;
};

}  // namespace

std::unique_ptr<Transport> make_transport() {
    return std::make_unique<WsTransport>();
}

}  // namespace pb2027::detail
