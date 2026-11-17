#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pb2027/client.hpp"

namespace pb2027::detail {

struct Inbound {
    enum class Kind : std::uint8_t { Binary, Ping, Pong, Close };
    Kind kind = Kind::Binary;
    std::vector<std::byte> data;  // binary payload or ping payload
    std::uint16_t close_code = 0;
    std::string close_reason;
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual std::expected<void, Error> connect(
        const ClientConfig& cfg,
        std::optional<TickNumber> since,
        std::chrono::milliseconds timeout) = 0;
    virtual std::expected<bool, Error> wait_readable(std::chrono::milliseconds timeout) = 0;
    virtual std::expected<Inbound, Error> read(std::chrono::milliseconds timeout) = 0;
    virtual std::expected<void, Error> send(std::span<const std::byte> payload) = 0;
    virtual std::expected<void, Error> send_pong(std::span<const std::byte> payload) = 0;
    virtual void close() noexcept = 0;
    virtual bool is_open() const noexcept = 0;
};

std::unique_ptr<Transport> make_transport();

}  // namespace pb2027::detail
