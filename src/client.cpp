#include "pb2027/client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "transport.hpp"

namespace pb2027 {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::uint64_t steady_now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch())
            .count());
}

bool retryable_close(std::uint16_t code) {
    switch (code) {
        case 1001:  // server shutdown
        case 1002:  // protocol error
        case 4000:  // malformed
        case 4002:  // too large
        case 4005:  // ip banned
            return false;
        default:
            return true;
    }
}

Error make_error(ErrorKind kind, std::string message) {
    return Error{kind, std::move(message), std::nullopt, std::nullopt};
}

}  // namespace

struct Client::Impl {
    struct Bucket {
        double tokens = 0.0;
        double rate = 0.0;      // messages per second
        double capacity = 1.0;
        Clock::time_point last = Clock::now();

        void configure(std::uint32_t rate_per_sec, std::uint32_t burst) {
            rate = static_cast<double>(rate_per_sec);
            capacity = static_cast<double>(burst == 0 ? 1 : burst);
            tokens = capacity;
            last = Clock::now();
        }

        void refill() {
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            if (rate <= 0.0) {
                return;
            }
            tokens = std::min(capacity, tokens + dt * rate);
        }

        bool take() {
            refill();
            if (tokens >= 1.0) {
                tokens -= 1.0;
                return true;
            }
            return false;
        }

        std::chrono::microseconds retry_after() {
            refill();
            if (rate <= 0.0) {
                return 1s;
            }
            const double missing = std::max(0.0, 1.0 - tokens);
            const double seconds = missing / rate;
            return std::chrono::microseconds(
                static_cast<std::int64_t>(seconds * 1'000'000.0));
        }
    };

    struct PendingBatch {
        Batch batch;
        SubmissionId id{};
        std::vector<RecordMeta> wire;
    };

    struct PendingRecord {
        RecordKind kind = RecordKind::Paint;
        SlotId slot = 0;
        SubmissionId submission{};
        std::uint32_t index = 0;
        PaintToken token{};
    };

    explicit Impl(ClientConfig c) : cfg(std::move(c)) {}

    ClientConfig cfg;
    Rules rules;
    State state = State::Idle;
    std::unique_ptr<detail::Transport> transport;

    std::function<void(const Rules&)> on_rules;
    std::function<void(const Tick&)> on_tick;
    std::function<void(const Snapshot&)> on_snapshot;
    std::function<void(const PublicBatch&)> on_public;
    std::function<void(const CloseInfo&)> on_close;
    std::function<void(const Error&)> on_error;

    std::deque<PendingBatch> outbound;
    Batch implicit;
    std::deque<PendingRecord> inflight;
    std::map<SlotId, PaintToken> desired;
    std::map<SlotId, PaintToken> active;
    std::map<SlotId, Brush> brushes;

    std::uint64_t next_submission = 1;
    std::optional<TickNumber> public_version;
    std::int64_t clock_offset_us = 0;
    Bucket bucket;

    void emit_error(const Error& e) {
        if (on_error) {
            on_error(e);
        }
    }

    codec::DecodeLimits limits() const {
        codec::DecodeLimits l;
        if (rules.max_message_bytes != 0) {
            l.max_message_bytes = rules.max_message_bytes;
        }
        if (rules.max_payload_bytes != 0) {
            l.max_payload_bytes = rules.max_payload_bytes;
        }
        if (rules.max_slots != 0) {
            l.max_slots = rules.max_slots;
        }
        if (rules.width != 0 && rules.height != 0) {
            // Downlink SNAPSHOT is 16 + 12*W*H bytes (codec 1) and is allowed
            // to exceed the client->server max_payload_bytes from RULES.
            const std::uint64_t snapshot =
                16ull + 12ull * static_cast<std::uint64_t>(rules.width) * rules.height;
            constexpr std::uint64_t kMaxField =
                std::numeric_limits<std::uint32_t>::max();
            if (snapshot > l.max_payload_bytes) {
                l.max_payload_bytes =
                    static_cast<std::uint32_t>(std::min(snapshot, kMaxField));
            }
            if (snapshot + 1024 > l.max_message_bytes) {
                l.max_message_bytes = static_cast<std::uint32_t>(
                    std::min(snapshot + 1024, kMaxField));
            }
        }
        l.width = rules.width;
        l.height = rules.height;
        return l;
    }

    codec::DecodeLimits pre_rules_limits() const {
        codec::DecodeLimits l;
        l.width = 0;
        l.height = 0;
        return l;
    }

    void apply_rules(const Rules& r) {
        rules = r;
        clock_offset_us =
            static_cast<std::int64_t>(r.server_time_us) -
            static_cast<std::int64_t>(steady_now_us());
        bucket.configure(r.msg_rate, r.msg_burst);
        if (on_rules) {
            on_rules(r);
        }
    }

    std::uint64_t server_now_us() const {
        const std::int64_t now =
            static_cast<std::int64_t>(steady_now_us()) + clock_offset_us;
        return now < 0 ? 0 : static_cast<std::uint64_t>(now);
    }

    std::size_t unsent_count() const {
        std::size_t n = implicit.size();
        for (const auto& b : outbound) {
            n += b.wire.size();
        }
        return n;
    }

    void enqueue(Batch&& batch) {
        PendingBatch pending;
        pending.batch = std::move(batch);
        pending.id = SubmissionId{next_submission++};
        pending.wire = pending.batch.wire_records();
        outbound.push_back(std::move(pending));
    }

    void enqueue_implicit() {
        if (implicit.empty()) {
            return;
        }
        Batch batch = std::move(implicit);
        implicit.clear();
        enqueue(std::move(batch));
    }

    void handle_close(std::uint16_t code, std::string reason) {
        if (transport) {
            transport->close();
            transport.reset();
        }
        state = State::Closed;
        CloseInfo info;
        info.code = code;
        info.reason = std::move(reason);
        info.retryable = retryable_close(code);
        info.unsent = unsent_count();
        info.ambiguous = inflight.size();
        outbound.clear();
        implicit.clear();
        inflight.clear();
        active.clear();
        brushes.clear();
        if (on_close) {
            on_close(info);
        }
    }

    void protocol_failure(std::string message) {
        Error e = make_error(ErrorKind::Protocol, message);
        emit_error(e);
        handle_close(1002, e.message);
    }

    void apply_tick(Tick& tick) {
        if (tick.results.size() > inflight.size()) {
            protocol_failure("TICK has more results than inflight records");
            return;
        }

        for (auto& result : tick.results) {
            PendingRecord record = inflight.front();
            inflight.pop_front();
            result.kind = record.kind;
            result.slot = record.slot;
            result.submission = record.submission;
            result.record = record.index;

            if (record.kind == RecordKind::Bind) {
                if (result.code == ResultCode::Ok) {
                    for (auto it = active.begin(); it != active.end();) {
                        if (it->second == record.token && it->first != record.slot) {
                            brushes.erase(it->first);
                            it = active.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    active[record.slot] = record.token;
                } else if (result.code == ResultCode::BadPaintToken ||
                           result.code == ResultCode::Banned) {
                    desired.erase(record.slot);
                }
            } else if (record.kind == RecordKind::Unbind) {
                if (result.code == ResultCode::Ok) {
                    active.erase(record.slot);
                    brushes.erase(record.slot);
                }
            }
        }

        for (SlotId slot : tick.lost) {
            active.erase(slot);
            desired.erase(slot);
            brushes.erase(slot);
        }
        for (const auto& update : tick.brushes) {
            if (active.contains(update.slot)) {
                brushes[update.slot] = update.brush;
            }
        }

        if (on_tick) {
            on_tick(tick);
        }
    }

    void dispatch(detail::Inbound& in) {
        switch (in.kind) {
            case detail::Inbound::Kind::Binary: {
                auto decoded = codec::decode_frame(in.data, limits());
                if (!decoded) {
                    protocol_failure(decoded.error().message);
                    return;
                }
                if (auto* r = std::get_if<Rules>(&*decoded)) {
                    apply_rules(*r);
                } else if (auto* t = std::get_if<Tick>(&*decoded)) {
                    apply_tick(*t);
                } else if (auto* s = std::get_if<Snapshot>(&*decoded)) {
                    public_version = s->version;
                    if (on_snapshot) {
                        on_snapshot(*s);
                    }
                } else if (auto* p = std::get_if<PublicBatch>(&*decoded)) {
                    public_version = p->to;
                    if (on_public) {
                        on_public(*p);
                    }
                }
                return;
            }
            case detail::Inbound::Kind::Ping: {
                if (auto r = transport->send_pong(in.data); !r) {
                    emit_error(r.error());
                    handle_close(0, "failed to send pong");
                }
                return;
            }
            case detail::Inbound::Kind::Pong:
                return;
            case detail::Inbound::Kind::Close:
                handle_close(in.close_code, std::move(in.close_reason));
                return;
        }
    }

    void close_transport() {
        if (transport) {
            transport->close();
            transport.reset();
        }
        state = State::Closed;
        active.clear();
        brushes.clear();
        outbound.clear();
        implicit.clear();
        inflight.clear();
    }
};

Client::Client(ClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

std::expected<void, Error> Client::connect() {
    auto& s = *impl_;
    if (s.state == State::Open || s.state == State::Connecting) {
        return std::unexpected(make_error(ErrorKind::InvalidArgument,
                                          "client is already connected"));
    }

    s.transport = detail::make_transport();
    s.state = State::Connecting;
    s.outbound.clear();
    s.implicit.clear();
    s.inflight.clear();
    s.active.clear();
    s.brushes.clear();

    const std::optional<TickNumber> since =
        s.cfg.public_stream ? s.public_version : s.cfg.since;
    auto connected =
        s.transport->connect(s.cfg, since, s.cfg.connect_timeout);
    if (!connected) {
        s.transport.reset();
        s.state = State::Closed;
        return std::unexpected(connected.error());
    }

    const auto deadline = Clock::now() + s.cfg.connect_timeout;
    while (Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            break;
        }
        auto ready = s.transport->wait_readable(remaining);
        if (!ready) {
            s.transport->close();
            s.transport.reset();
            s.state = State::Closed;
            return std::unexpected(ready.error());
        }
        if (!*ready) {
            continue;
        }

        auto in = s.transport->read(s.cfg.read_timeout);
        if (!in) {
            s.transport->close();
            s.transport.reset();
            s.state = State::Closed;
            return std::unexpected(in.error());
        }
        if (in->kind == detail::Inbound::Kind::Ping) {
            (void)s.transport->send_pong(in->data);
            continue;
        }
        if (in->kind == detail::Inbound::Kind::Pong) {
            continue;
        }
        if (in->kind == detail::Inbound::Kind::Close) {
            s.handle_close(in->close_code, std::move(in->close_reason));
            return std::unexpected(make_error(ErrorKind::Closed,
                                              "server closed before RULES"));
        }

        auto decoded = codec::decode_frame(in->data, s.pre_rules_limits());
        if (!decoded) {
            s.protocol_failure(decoded.error().message);
            return std::unexpected(decoded.error());
        }
        if (auto* r = std::get_if<Rules>(&*decoded)) {
            s.apply_rules(*r);
            s.state = State::Open;
            return {};
        }
        // SNAPSHOT/PUBLIC are not expected before RULES; ignore.
    }

    s.transport->close();
    s.transport.reset();
    s.state = State::Closed;
    return std::unexpected(make_error(ErrorKind::Timeout, "timed out waiting for RULES"));
}

std::expected<void, Error> Client::reconnect() {
    auto& s = *impl_;
    if (s.state == State::Idle) {
        return connect();
    }
    if (s.state == State::Open) {
        s.close_transport();
    }
    auto result = connect();
    if (!result) {
        return result;
    }
    for (const auto& [slot, token] : s.desired) {
        s.implicit.bind(slot, token);
    }
    (void)flush();
    return {};
}

void Client::close() {
    impl_->close_transport();
}

void Client::on_rules(std::function<void(const Rules&)> handler) {
    impl_->on_rules = std::move(handler);
}
void Client::on_tick(std::function<void(const Tick&)> handler) {
    impl_->on_tick = std::move(handler);
}
void Client::on_snapshot(std::function<void(const Snapshot&)> handler) {
    impl_->on_snapshot = std::move(handler);
}
void Client::on_public(std::function<void(const PublicBatch&)> handler) {
    impl_->on_public = std::move(handler);
}
void Client::on_close(std::function<void(const CloseInfo&)> handler) {
    impl_->on_close = std::move(handler);
}
void Client::on_error(std::function<void(const Error&)> handler) {
    impl_->on_error = std::move(handler);
}

State Client::state() const noexcept {
    return impl_->state;
}
const Rules& Client::rules() const noexcept {
    return impl_->rules;
}
std::uint64_t Client::server_now_us() const noexcept {
    return impl_->server_now_us();
}
std::optional<TickNumber> Client::public_version() const noexcept {
    return impl_->public_version;
}
std::optional<Brush> Client::brush(SlotId slot) const {
    auto it = impl_->brushes.find(slot);
    if (it == impl_->brushes.end()) {
        return std::nullopt;
    }
    return it->second;
}
std::optional<PaintToken> Client::active_token(SlotId slot) const {
    auto it = impl_->active.find(slot);
    if (it == impl_->active.end()) {
        return std::nullopt;
    }
    return it->second;
}
std::optional<std::uint64_t> Client::ready_at_us(SlotId slot, PixelIndex idx) const {
    const auto& s = *impl_;
    if (!s.active.contains(slot)) {
        return std::nullopt;
    }
    auto it = s.brushes.find(slot);
    if (it == s.brushes.end() || !it->second.idx.has_value()) {
        return 0;  // empty brush: no cooldown
    }

    if (s.rules.width == 0 || s.rules.height == 0) {
        return 0;
    }
    const Brush& brush = it->second;
    const std::int64_t x1 = static_cast<std::int64_t>(idx % s.rules.width);
    const std::int64_t y1 = static_cast<std::int64_t>(idx / s.rules.width);
    const std::int64_t x0 =
        static_cast<std::int64_t>(*brush.idx % s.rules.width);
    const std::int64_t y0 =
        static_cast<std::int64_t>(*brush.idx / s.rules.width);
    const std::uint64_t distance = static_cast<std::uint64_t>(
        std::llabs(x1 - x0) + std::llabs(y1 - y0));

    std::uint64_t delay = s.rules.cooldown_base_us;
    if (s.rules.cooldown_per_px_us != 0 &&
        distance > (std::numeric_limits<std::uint64_t>::max() - delay) /
                       s.rules.cooldown_per_px_us) {
        delay = std::numeric_limits<std::uint64_t>::max();
    } else {
        delay += s.rules.cooldown_per_px_us * distance;
    }
    if (brush.t_us > std::numeric_limits<std::uint64_t>::max() - delay) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return brush.t_us + delay;
}
std::size_t Client::inflight_records() const noexcept {
    return impl_->inflight.size();
}

void Client::bind(SlotId slot, PaintToken token) {
    auto& s = *impl_;
    if (s.rules.max_slots != 0 && slot >= s.rules.max_slots) {
        s.emit_error(make_error(ErrorKind::InvalidArgument, "bind slot out of range"));
        return;
    }
    for (auto it = s.desired.begin(); it != s.desired.end();) {
        if (it->second == token && it->first != slot) {
            it = s.desired.erase(it);
        } else {
            ++it;
        }
    }
    s.desired[slot] = token;
    if (s.state == State::Open) {
        s.implicit.bind(slot, token);
    }
}

void Client::unbind(SlotId slot) {
    auto& s = *impl_;
    s.desired.erase(slot);
    if (s.state == State::Open) {
        s.implicit.unbind(slot);
    }
}

void Client::paint(SlotId slot, PixelIndex idx, Color color) {
    auto& s = *impl_;
    if (s.state != State::Open) {
        s.emit_error(make_error(ErrorKind::Closed, "paint ignored: client not open"));
        return;
    }
    s.implicit.paint(slot, idx, color);
}

std::expected<SubmissionId, Error> Client::submit(Batch&& batch) {
    auto& s = *impl_;
    if (s.state != State::Open) {
        return std::unexpected(make_error(ErrorKind::Closed, "client is not open"));
    }
    if (batch.empty()) {
        return std::unexpected(make_error(ErrorKind::InvalidArgument, "empty batch"));
    }
    s.enqueue_implicit();
    s.enqueue(std::move(batch));
    return s.outbound.back().id;
}

PollStatus Client::poll(std::chrono::milliseconds timeout) {
    auto& s = *impl_;
    if (s.state == State::Closed) {
        return PollStatus::Closed;
    }
    if (s.state != State::Open || !s.transport) {
        return PollStatus::Timeout;
    }

    const auto deadline = Clock::now() + timeout;
    std::size_t processed = 0;
    while (processed < s.cfg.max_messages_per_poll) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining <= std::chrono::milliseconds::zero()) {
            break;
        }
        auto ready = s.transport->wait_readable(remaining);
        if (!ready) {
            s.emit_error(ready.error());
            s.handle_close(0, ready.error().message);
            return PollStatus::Closed;
        }
        if (!*ready) {
            break;
        }

        auto in = s.transport->read(s.cfg.read_timeout);
        if (!in) {
            s.emit_error(in.error());
            s.handle_close(0, in.error().message);
            return PollStatus::Closed;
        }
        s.dispatch(*in);
        ++processed;
        if (s.state == State::Closed) {
            return PollStatus::Closed;
        }
    }
    return processed == 0 ? PollStatus::Timeout : PollStatus::Ok;
}

Client::FlushResult Client::flush() {
    auto& s = *impl_;
    FlushResult result;
    if (s.state != State::Open || !s.transport) {
        return result;
    }
    s.enqueue_implicit();
    if (s.outbound.empty()) {
        return result;
    }

    if (!s.bucket.take()) {
        result.retry_after = s.bucket.retry_after();
        return result;
    }

    auto& front = s.outbound.front();
    auto frame = codec::encode_batch(front.batch, s.rules, s.cfg.zstd_level);
    if (!frame) {
        s.emit_error(frame.error());
        s.outbound.pop_front();
        return result;
    }

    auto sent = s.transport->send(*frame);
    if (!sent) {
        s.emit_error(sent.error());
        s.handle_close(0, "send failed");
        return result;
    }

    const auto binds = front.batch.bind_records();
    for (std::size_t i = 0; i < front.wire.size(); ++i) {
        Impl::PendingRecord record;
        record.kind = front.wire[i].kind;
        record.slot = front.wire[i].slot;
        record.submission = front.id;
        record.index = static_cast<std::uint32_t>(i);
        if (record.kind != RecordKind::Paint && i < binds.size()) {
            record.token = binds[i].token;
        }
        s.inflight.push_back(std::move(record));
    }

    result.sent = true;
    result.id = front.id;
    result.records = front.wire.size();
    result.bytes = frame->size();
    s.outbound.pop_front();
    return result;
}

}  // namespace pb2027
