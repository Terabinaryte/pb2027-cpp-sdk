#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "pb2027/codec.hpp"
#include "pb2027/error.hpp"
#include "pb2027/types.hpp"

namespace pb2027 {

enum class State : std::uint8_t { Idle, Connecting, Open, Closed };
enum class PollStatus : std::uint8_t { Ok, Timeout, Closed };

struct ClientConfig {
    std::string host;
    std::uint16_t port = 443;
    bool tls = true;
    bool verify_peer = true;
    std::string ca_file;  // empty = system CA
    bool public_stream = false;
    std::optional<TickNumber> since;
    std::optional<ProxyConfig> proxy;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds read_timeout{1000};
    std::chrono::milliseconds write_timeout{1000};
    std::size_t max_messages_per_poll = 64;
    int zstd_level = 3;
    std::function<void(LogLevel, std::string_view)> logger;
};

struct CloseInfo {
    std::uint16_t code = 0;
    std::string reason;
    bool retryable = false;
    std::size_t unsent = 0;
    std::size_t ambiguous = 0;
};

class Client {
public:
    explicit Client(ClientConfig config);
    ~Client();
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    std::expected<void, Error> connect();
    std::expected<void, Error> reconnect();
    void close();

    void on_rules(std::function<void(const Rules&)> handler);
    void on_tick(std::function<void(const Tick&)> handler);
    void on_snapshot(std::function<void(const Snapshot&)> handler);
    void on_public(std::function<void(const PublicBatch&)> handler);
    void on_close(std::function<void(const CloseInfo&)> handler);
    void on_error(std::function<void(const Error&)> handler);

    State state() const noexcept;
    const Rules& rules() const noexcept;
    std::uint64_t server_now_us() const noexcept;
    std::optional<TickNumber> public_version() const noexcept;
    std::optional<Brush> brush(SlotId slot) const;
    std::optional<PaintToken> active_token(SlotId slot) const;
    std::optional<std::uint64_t> ready_at_us(SlotId slot, PixelIndex idx) const;
    std::size_t inflight_records() const noexcept;

    void bind(SlotId slot, PaintToken token);
    void unbind(SlotId slot);
    void paint(SlotId slot, PixelIndex idx, Color color);
    std::expected<SubmissionId, Error> submit(Batch&& batch);

    PollStatus poll(std::chrono::milliseconds timeout);

    struct FlushResult {
        bool sent = false;
        SubmissionId id{};
        std::size_t records = 0;
        std::size_t bytes = 0;
        std::optional<std::chrono::microseconds> retry_after;
    };
    FlushResult flush();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pb2027
