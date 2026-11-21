# pb2027 C++ SDK 文档

> 版本：0.1.0（M0–M6 已完成）
> 协议：`../protocol.md`（SHA-256 `25ccf58a…`）
> 适用：Linux x86_64、C++23、GCC 13+/Clang 16+
> 更新：2026-11-17

---

## 目录

1. [概览](#1-概览)
2. [构建与安装](#2-构建与安装)
3. [快速开始](#3-快速开始)
4. [核心概念](#4-核心概念)
5. [API 参考](#5-api-参考)
6. [运行时契约](#6-运行时契约)
7. [真机差异与注意事项](#7-真机差异与注意事项)
8. [示例、测试与性能](#8-示例测试与性能)
9. [已知限制与优化建议](#9-已知限制与优化建议)
10. [版本与兼容性](#10-版本与兼容性)

---

## 1. 概览

pb2027 C++ SDK 是 ikapb 协议的 C++23 客户端库，覆盖：

| 能力 | 目标 |
| --- | --- |
| 协议编解码 | `BATCH` 编码；`RULES` / `TICK` / `SNAPSHOT` / `PUBLIC` 解码；严格校验 |
| 会话 | TLS WebSocket、槽位绑定、绘画、结果关联、Ping/Pong、关闭、重连 |
| 观战 | 公共画面 + 两层历史 + 声纳叠加（`Board` / `SonarTable`） |
| HTTP（可选） | `/api/meta`、`/api/paint-token`、`/api/board` |

### Target 划分

| target | 依赖 | 用途 |
| --- | --- | --- |
| `pb2027::codec` | zstd | 纯协议层，无网络 |
| `pb2027::client` | codec + websocketclient-cpp + OpenSSL + zlib | `Client` |
| `pb2027::viewer` | codec | `Board` / `SonarTable` |
| `pb2027::api` | codec + cpp-httplib + OpenSSL + zstd | HTTP 模块（可选） |

公共头只依赖标准库，不泄漏 OpenSSL / zstd / httplib / websocketclient 类型。

### 当前不支持

- 非 TLS 的 `ws://`、WS 代理 CONNECT（`ClientConfig::proxy` 会返回 `Unsupported`）
- `/admin/ws` 管理员实时流
- 像素渲染 / GUI / 播放器
- Windows（blocking transport 基于 POSIX）

---

## 2. 构建与安装

### 2.1 依赖

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| CMake | ≥ 3.21 | 构建 |
| C++ | C++23 | `std::expected` 等 |
| zstd | ≥ 1.5 | `find_package(zstd CONFIG)` |
| OpenSSL | 3 / 4 | TLS |
| zlib | 任意 | websocketclient-cpp 的硬依赖 |
| Python | 3.14+ | 仅测试用；当前 CMake 会无条件查找 |

第三方头文件已 vendor 在 `third_party/`，构建不需要联网。

### 2.2 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

可选：

```bash
cmake -S . -B build \
  -DPB2027_BUILD_TESTS=OFF \
  -DPB2027_BUILD_EXAMPLES=OFF \
  -DPB2027_ENABLE_NETWORK_TESTS=ON
```

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `PB2027_BUILD_TESTS` | ON | 构建 codec / board / client / api 测试 |
| `PB2027_BUILD_EXAMPLES` | ON | 构建 `real_probe` / `viewer_demo` / `ws_smoke` / `paint_probe` / `paint_rect` |
| `PB2027_ENABLE_NETWORK_TESTS` | OFF | 把 `pb2027_api_tests` 加入 ctest（会请求真实 `/api/meta`） |

### 2.3 安装与消费

```bash
cmake --install build --prefix /opt/pb2027
```

```cmake
find_package(pb2027 CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE
    pb2027::codec
    pb2027::client
    pb2027::viewer
    pb2027::api      # 可选
)
```

安装包会导出 `pb2027::codec` / `client` / `viewer` / `api`，并传递 OpenSSL、zlib、zstd 链接依赖。

> 画笔调度器不在 SDK 内：SDK 只提供 `Client` / `Board` 原语，调度与多 token 策略在独立仓库 `pb2027-cpp-client`（稍后开源）。

---

## 3. 快速开始

### 3.1 只读观战

```cpp
#include <pb2027/board.hpp>
#include <pb2027/client.hpp>

using namespace pb2027;
using namespace std::chrono_literals;

int main() {
    ClientConfig cfg;
    cfg.host = "pb.ika.zip";
    cfg.port = 443;
    cfg.tls = true;
    cfg.verify_peer = true;
    cfg.public_stream = true;          // 收 SNAPSHOT / PUBLIC
    cfg.connect_timeout = 10s;
    cfg.read_timeout = 10s;

    Board board;
    Client client(cfg);

    client.on_snapshot([&](const Snapshot& s) {
        if (auto r = board.apply(s); !r) { /* log */ }
    });
    client.on_public([&](const PublicBatch& p) {
        if (auto r = board.apply(p); !r) { /* log */ }
    });

    if (auto r = client.connect(); !r) return 1;

    while (client.state() == State::Open) {
        client.poll(200ms);
        if (board.initialized()) {
            if (auto c = board.color_at(0)) {
                // c->v 是 0xAABBGGRR
            }
        }
    }
}
```

### 3.2 用现成 token 绘画

```cpp
ClientConfig cfg;
cfg.host = "pb.ika.zip";
cfg.port = 443;
cfg.public_stream = false;             // 工作连接不需要公共流
cfg.connect_timeout = 10s;
cfg.read_timeout = 10s;

Client client(cfg);
client.on_tick([](const Tick& t) {
    for (const auto& r : t.results) {
        // r.code, r.value, r.kind, r.slot, r.submission, r.record
    }
});
client.on_close([](const CloseInfo& c) {
    // c.code / c.retryable / c.unsent / c.ambiguous
});

if (auto r = client.connect(); !r) return 1;

auto token = PaintToken::from_uuid("00010203-0405-0607-0809-0a0b0c0d0e0f");
if (!token) return 1;

client.bind(0, *token);
client.paint(0, 100 * 1000 + 200, Color::rgba(255, 255, 255));
if (!client.flush().sent) { /* rate limited, see retry_after */ }

const auto period = std::chrono::microseconds(1'000'000 / client.rules().tps);
auto next = std::chrono::steady_clock::now();
while (client.state() == State::Open) {
    client.poll(2ms);        // 只收：排空 + 回 Pong
    client.flush();          // 发一条 BATCH
    next += period;
    std::this_thread::sleep_until(next);
}
```

### 3.3 申请 token

```cpp
#include <pb2027/api.hpp>

api::Endpoint ep;
ep.host = "pb.ika.zip";
ep.port = 443;
ep.tls = true;
ep.verify_peer = true;

// 洛谷模式：access key
auto issued = api::acquire_token(ep, api::AccessKeyRequest{"<43-char-key>"});

// 测试模式（issuance.access_mode == "none"）：服务器要求 uid
auto test_token = api::acquire_token(ep, api::UidRequest{900001});
```

`issued->uid`、`issued->token`、`issued->access_key`（可能为空）。

### 3.4 只用 codec

```cpp
#include <pb2027/codec.hpp>

Batch batch;
batch.paint(0, 12345, Color::rgba(1, 2, 3));

auto frame = codec::encode_batch(batch, rules);   // 单个 zstd frame
if (!frame) { /* Error */ }

codec::DecodeLimits limits;
limits.width = rules.width;
limits.height = rules.height;
auto decoded = codec::decode_frame(*frame, limits);  // variant<Rules, Tick, ...>
```

---

## 4. 核心概念

| 概念 | 说明 |
| --- | --- |
| `Uid` | 账号 id，`u32`；冷却、画笔按 uid 计算 |
| `PaintToken` | 16 字节凭据；文本形式是 UUID；一个 uid 同时只有一个有效 token |
| `SlotId` | 连接内的 0…`max_slots-1` 槽位；绑定 token 后绘画只带槽号 |
| `Brush` | 属于 uid 的 `(idx, t_us)`；空画笔表示还没有成功落笔 |
| `Tick` | 服务器每 tick 推送的结果、画笔、声纳、`lost.slot` |
| `SubmissionId` | SDK 本地编号；用于把 `Result` 关联回 `submit()`/`flush()` 的批次 |
| `Vpub` | 公共画面版本（tick 号） |
| `l1/l2` | 公共画面的两层历史颜色；`PUBLIC` 颜色码 0/1 引用它们 |

### 4.1 结果关联

协议没有请求 ID，结果只按连接内记录顺序返回。SDK 的规则：

1. `bind/unbind/paint` 先进入 **outbound**（隐式批次 / `submit()`）。
2. `flush()` 成功写出后才把记录放入 **inflight**。
3. `TICK.nr` 个结果按顺序从 `inflight` 出队，回填 `kind/slot/submission/record`。
4. `nr > inflight` 是协议错误；`nr < inflight` 正常（剩余留到后续 tick）。

### 4.2 槽位状态

- `desired_slots_`：用户希望绑定什么；`reconnect()` 时自动重发。
- `active_slots_`：服务器已确认绑定什么；只有 `Bind + OK` 才更新。
- `lost.slot`：清空 active 与 desired（四种原因都是终态），不自动重绑。
- `BAD_PAINT_TOKEN`/`BANNED`：移除 desired；`IP_LIMIT`：保留 desired，但不自动重试。

### 4.3 冷却

```
d = |x_target - x_brush| + |y_target - y_brush|
ready = brush.t_us + cooldown_base_us + cooldown_per_px_us * d
```

真机当前参数：`cooldown_base_us = 1 s`、`cooldown_per_px_us = 0.1 s/px`；文档默认值是 10 s / 1 s。
`Client::ready_at_us()` 返回目标像素可落笔的服务器时间；空画笔返回 0。

---

## 5. API 参考

### 5.1 `types.hpp`

```cpp
using Uid = std::uint32_t;
using SlotId = std::uint16_t;
using PixelIndex = std::uint32_t;
using TickNumber = std::uint64_t;

struct Color { std::uint32_t v; static Color rgba(r,g,b); r(); g(); b(); };
struct PaintToken { std::array<std::byte,16> bytes;
    static std::optional<PaintToken> from_uuid(std::string_view);
    std::string to_uuid() const; };

struct Rules {
    std::uint16_t width, height;
    std::uint16_t public_tps;        // 真机 offset 2；文档旧版称保留
    std::uint16_t sonar_radius_px;
    std::uint32_t tps;
    std::uint64_t cooldown_base_us, cooldown_per_px_us;
    std::uint64_t public_delay_ms, public_period_ms;
    std::uint64_t event_start_unix_ms, event_end_unix_ms;
    std::uint32_t max_slots, max_paints_per_message;
    std::uint32_t msg_rate, msg_burst, max_message_bytes, max_payload_bytes;
    std::uint64_t server_time_us;
};

enum class ResultCode : std::uint8_t {
    Ok=0, Cooling=1, NoPaintToken=2, BadPaintToken=3,
    Banned=4, Closed=5, IpLimit=6,
};

enum class RecordKind : std::uint8_t { Bind, Unbind, Paint };

struct Result {
    ResultCode code;
    std::uint32_t value;       // Cooling: 剩余微秒；IpLimit: 上限
    RecordKind kind;
    SlotId slot;
    SubmissionId submission;
    std::uint32_t record;      // 批次内 wire 顺序下标
};

struct Tick { TickNumber number; vector<Result> results;
              vector<BrushUpdate> brushes; vector<Sonar> sonars;
              vector<SlotId> lost; };

struct Snapshot { uint16_t width, height; uint64_t version;
                  SnapshotCodec codec; vector<Color> pixels, l1, l2; };

struct PublicEntry { std::uint32_t key; std::uint32_t color; };
struct PublicSegment { std::uint32_t tick; vector<PublicEntry> entries; };
struct PublicBatch { uint16_t sample_rate; uint64_t from, to;
                     uint32_t order; vector<PublicSegment> segments; };
```

### 5.2 `error.hpp`

```cpp
enum class ErrorKind {
    Transport, Tls, Handshake, Protocol, Timeout, Closed,
    RateLimited, Banned, InvalidArgument, Unsupported, Internal
};

struct Error {
    ErrorKind kind;
    std::string message;
    std::optional<std::uint16_t> close_code;
    std::optional<std::chrono::seconds> retry_after;
};

std::string to_string(const Error&);
```

所有可能失败的操作返回 `std::expected<T, Error>`；不抛异常。

### 5.3 `codec.hpp`

`Batch`：

```cpp
Batch& bind(SlotId, PaintToken);
Batch& unbind(SlotId);
Batch& paint(SlotId, PixelIndex, Color);
std::size_t size() const;
std::vector<RecordMeta> wire_records() const;  // 绑定/解绑在前，绘画在后
```

`codec`：

```cpp
std::expected<std::vector<std::byte>, Error>
encode_batch(const Batch&, const Rules&, int level = 3);

std::expected<DecodedMessage, Error>
decode_frame(std::span<const std::byte> frame, const DecodeLimits&);

std::expected<DecodedMessage, Error>
decode_payload(std::span<const std::byte> payload, const DecodeLimits&);
```

`DecodedMessage = variant<Rules, Snapshot, PublicBatch, Tick, UnknownMessage>`。
`DecodeLimits` 的 `width/height` 为 0 表示跳过像素范围校验；`PUBLIC` 保留 wire key，不需要宽高。

`encode_batch` 会校验槽位、`idx`、alpha、`max_slots`、`max_paints_per_message`、压缩后尺寸；输出恰好一个 zstd frame。

`decode_frame` 会校验：单 frame、无字典、窗口 ≤ 8 MiB、解压上限、各消息的精确长度、保留字段、升序、alpha、`Σ seg.n`、`dsmall=255`/`dbig` 数量等。

### 5.4 `client.hpp`

```cpp
struct ClientConfig {
    std::string host;
    std::uint16_t port = 443;
    bool tls = true;                 // 目前只支持 true
    bool verify_peer = true;
    std::string ca_file;
    bool public_stream = false;
    std::optional<TickNumber> since;
    std::optional<ProxyConfig> proxy; // 目前返回 Unsupported
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds read_timeout{1000};
    std::chrono::milliseconds write_timeout{1000}; // 预留，当前未使用
    std::size_t max_messages_per_poll = 64;
    int zstd_level = 3;
    std::function<void(LogLevel, std::string_view)> logger; // 预留，当前未接线
};
```

`Client`：

| 方法 | 说明 |
| --- | --- |
| `connect()` | 建连 + 握手 + 等到 `RULES`；阻塞 |
| `reconnect()` | `close` + `connect` + 重绑 `desired` + `since`；阻塞到 `RULES` |
| `poll(timeout)` | 只收：排空、回 Pong、处理关闭；总 deadline；不建连 |
| `flush()` | 至多发送一条 BATCH；自带 `msg_rate/msg_burst` 令牌桶 |
| `bind/unbind/paint` | 更新期望/入队；不直接写 socket |
| `submit(Batch&&)` | 显式提交整条 BATCH，返回 `SubmissionId` |
| `brush(slot)` | 服务器推送的权威画笔 |
| `active_token(slot)` | 已确认绑定的 token |
| `ready_at_us(slot, idx)` | 冷却就绪时间；空画笔返回 0 |
| `server_now_us()` | 已用 `RULES.server_time_us` 校准 |
| `public_version()` | 已应用到的 `Vpub` |

回调：`on_rules` / `on_tick` / `on_snapshot` / `on_public` / `on_close` / `on_error`。
每个回调只能设置一个处理函数；回调在 `poll()` 线程内同步调用。

`CloseInfo`：`code`、`reason`、`retryable`、`unsent`、`ambiguous`。
4000/4002/4005/1001 为非 retryable；4003/4004/传输错误为 retryable。

`FlushResult`：

```cpp
struct FlushResult {
    bool sent;
    SubmissionId id;
    std::size_t records, bytes;
    std::optional<std::chrono::microseconds> retry_after;
};
```

### 5.6 `board.hpp`

```cpp
enum class ColorSource { Public, Sonar };
struct ResolvedColor {
    Color color;
    ColorSource source;
    TickNumber tick;
};

class Board {
    Board();
    void reset(std::uint16_t width, std::uint16_t height);
    std::expected<void, Error> apply(const Snapshot&);
    std::expected<void, Error> apply(const PublicBatch&);
    void overlay(const Sonar&, TickNumber);
    void prune(TickNumber public_version);
    bool initialized() const;
    std::uint16_t width() const, height() const;
    TickNumber version() const;
    std::optional<Color> color_at(PixelIndex) const;
    std::optional<Color> public_color_at(PixelIndex) const;
    std::optional<std::pair<Color, TickNumber>> sonar_at(PixelIndex) const;
    std::optional<ResolvedColor> resolve_color(PixelIndex, TickNumber min_sonar_tick) const;
    void observe(const Tick& tick);
    std::span<const Color> pixels() const;
};

class SonarTable {
    SonarTable();
    SonarTable(std::uint16_t width, std::uint16_t height);
    void reset(std::uint16_t width, std::uint16_t height);
    void add(const Sonar&, TickNumber);
    void prune(TickNumber);
    std::optional<std::pair<Color, TickNumber>> at(PixelIndex) const;
};
```

- `apply(Snapshot)` 重建画面/历史层并清空声纳；codec `FullWithHistory` 时 `l1/l2` 必须恰好 `W*H`。
- `apply(PublicBatch)` 需要已初始化；`order=0` 时 `idx=key`，`order=1` 时 `idx=(key % H)*W + key/H`；颜色码 0/1/2 按 §3.7.3 处理。
- 无历史层时（只 `reset()` 或 codec `Full`）遇到 0/1 返回 `InvalidArgument`。
- `pixels()` 在 `reset`/`apply(Snapshot)` 后失效。

### 5.6.1 画笔位置的本地视野（sonar 是观测，不是真值）

`Client` 不会自动把 `TICK.sonars` 喂给 `Board`；这是一层显式组合：

```cpp
Board board;
board.reset(client.rules().width, client.rules().height);  // 工作连接没有公共流时先置尺寸

client.on_tick([&](const Tick& t) {
    board.observe(t);          // overlay 本 tick 的 sonar，并按 version() prune
});

// 查询时显式给出“sonar 至少要多新”：
if (auto brush = client.brush(slot); brush && brush->idx) {
    auto resolved = board.resolve_color(*brush->idx, min_sonar_tick);
    if (resolved) {
        // resolved->source == Sonar 或 Public，resolved->tick 是对应层的时间
    }
}
```

**不要**把 sonar 直接合并进 public 真值。规则应该是：

1. `sonar_at(idx)` 存在且 `sonar.tick >= min_sonar_tick` → 用 sonar，来源标为 `ColorSource::Sonar`；
2. 否则用公共画面，来源标为 `ColorSource::Public`；
3. 两者都没有 → `nullopt`，表示“不知道”，不是黑色。

`min_sonar_tick` 由调用方决定。典型值：
- 渲染/观察：`min_sonar_tick = current_tick - 2*probe_interval_ticks`；
- 攻击检测：只接受“比上次写入更新”的 sonar，过期必须回退 public，不能让旧 sonar 抑制差异。

注意：声纳只发给**发出该笔的连接**，同一 token 在其他连接/网页上画的内容不会变成你的声纳；`observe` 只处理本连接收到的 `Tick.sonars`。

`observe()` 还会按 **1024 tick（约 8 秒 @128 tps）** 修剪 sonar 历史，不只依赖延迟的 public version；否则只有 painter 连接、public 不前进时，sonar FIFO 会持续增长。多连接时每个 painter 连接使用独立 `Board`，或给 `SonarObservation` 增加 source 后过滤。

### 5.7 `api.hpp`

```cpp
struct Endpoint {
    std::string host; std::uint16_t port = 443;
    bool tls = true, verify_peer = true;
    std::string ca_file, base_path = "/";
    std::optional<ProxyConfig> proxy;
    std::optional<std::string> proxy_username, proxy_password;
    std::chrono::milliseconds request_timeout{10000};
};

std::expected<Meta, Error> fetch_meta(const Endpoint&);
std::expected<IssuedToken, Error> acquire_token(const Endpoint&, TokenRequest);
std::expected<BoardResponse, Error> fetch_board(const Endpoint&,
                                                std::optional<std::uint64_t> since);
```

`TokenRequest = variant<AccessKeyRequest, CaptchaRequest, UidRequest>`。
`BoardResponse`：`Kind::{NotModified, Snapshot, Public}` + `version` + `snapshot`/`batches`。

`fetch_board` 内部完成：HTTP → zstd 解压 → 判断 `0x83`/`0x82` → 多段 PUBLIC 按长度切分 → codec 解码；解码上限 64 MiB（适配真机 7.2 MB 快照）。

---

## 6. 运行时契约

### 6.1 线程

- 所有 API 必须在**同一线程**调用；`Client` 不创建内部线程、不加锁。
- `poll()` 内派发回调；回调里可以 `paint/bind/submit`（入队），但**不要**再次调用 `poll/flush/reconnect`。
- 回调不得抛异常。
- `Board` 与 `Client` 同线程。

### 6.2 生命周期

- `Client`/`Board` 均可移动、不可拷贝。
- 回调参数只在回调期间有效；`Tick`/`PublicBatch` 是值语义，可拷贝留存。

### 6.3 错误与关闭

- 显式 `close()`：只关闭 socket 并清空队列，**不触发 `on_close`**。
- 远端 Close / 传输错误：触发 `on_close`，并给出 `unsent` / `ambiguous` 计数。
- `paint()` 在非 `Open` 状态会丢弃并触发 `on_error`；`bind/unbind` 只更新期望表。
- `flush()` 编码失败会丢弃该批并 `on_error`（不会自动重试）。
- `flush()` 的 `retry_after` 表示令牌桶还差多久；调用方应在下一 tick 重试。

### 6.4 下行与背压

- `poll(timeout)` 的总耗时不严格等于 `timeout`：单条消息读取受 `read_timeout` 限制；慢网络/大 SNAPSHOT 下可能略超。
- 必须持续 `poll()`，即使不关心 `TICK`，否则可能触发 16 MiB / 10 s 下行积压（`4003`）。
- `max_messages_per_poll` 防止公共流饿死 `flush()`。

### 6.5 重连

- SDK **不会自动重连**；`poll()` 返回 `Closed` / `on_close` 后由调用方退避并调用 `reconnect()`。
- `reconnect()` 重发 `desired_slots_`，`since` 用 `public_version()`；`Board` 的 `l1/l2` 需要调用方保留（`Board` 自己持有即可）。
- `unsent` 的 paint 丢弃，`ambiguous` 的 paint 不重发（可能已生效）；调度器应重新决策。

---

## 7. 真机差异与注意事项

1. **RULES offset 2**：`protocol.md` 写“保留 0”，真机是 `0x0020`（32），即 `public.tps`。SDK 已暴露为 `Rules.public_tps` 并接受非零。
2. **下行 SNAPSHOT 大小**：1000×600 codec 1 解压后 `7,200,016` 字节、压缩后约 `1.32 MB`；`RULES.max_payload_bytes = 1 MiB` 只适用于**客户端上行**。SDK 下行按 `16 + 12·W·H` 放宽，WS 读缓冲 16 MiB。
3. **真实参数**：`max_slots=8000`、`cooldown_base_us=1 s`、`cooldown_per_px_us=0.1 s/px`、`public_delay_ms=60 s`。不要按文档默认值硬编码。
4. **`public.tps`** 不在 `/api/meta` 里；只能从 `RULES` 的 `public_tps` 或 `PUBLIC.sample_rate` 得知。
5. **`/api/board`** 返回一个 zstd frame；解压后可能是 codec 1 Snapshot，也可能是多条 PUBLIC 首尾相接；`api::fetch_board` 已处理。

---

## 8. 示例、测试与性能

### 8.1 示例

| 示例 | 用途 |
| --- | --- |
| `pb2027_real_probe` | 真机只读：RULES / SNAPSHOT / PUBLIC |
| `pb2027_viewer_demo` | 真机只读：`Board` 应用公共画面 |
| `pb2027_ws_smoke` | 本地 TLS WS echo 冒烟 |
| `pb2027_paint_probe` | 测试服：批量 token、绑定、落笔、冷却、重连 |
| `pb2027_paint_rect` | 测试服：用 300 token 画 50×50 矩形 |

### 8.2 测试

```bash
ctest --test-dir build --output-on-failure
```

| 测试 | 用例 | 断言 |
| --- | --- | --- |
| `pb2027_codec_tests` | 15 | 116 |
| `pb2027_board_tests` | 22 | 186 |
| `pb2027_client_tests` | 2 | 55 |
| `pb2027_api_tests` | 10 | 133（需 `PB2027_ENABLE_NETWORK_TESTS=ON`） |

`pb2027_client_tests` 会启动本地 TLS mock server（Python + 自签证书），不依赖外网。

### 8.3 性能参考

- 300 token、2500 像素、9 波：**32.4 s**，全部 OK，约 78 像素/秒。
- 300 token 并行申请（16 路）：约 15 s；`acquire_token` 每次新建 HTTPS 连接，这是主要成本。
- 真机 SNAPSHOT 解码 7.2 MB；`Client` 按 frame content-size 分配，TICK 不会反复分配大缓冲。

---

## 9. 已知限制与优化建议

按优先级排序，供后续迭代：

### P0：影响正确性/可运维性

| 问题 | 现状 | 建议 |
| --- | --- | --- |
| `flush()` 不拆批 | 隐式/显式批次超过 `max_paints_per_message` 或 1 MiB 时整批编码失败并丢弃 | 在 `Client` 内按列限制拆成多条 BATCH；`FlushResult` 返回剩余量 |
| 日志/写超时未接线 | `ClientConfig::logger`、`write_timeout` 还没有传给 transport | 把 logger 适配到 websocketclient 的 `ConsoleLogger`，send 使用写超时 |
| 代理/非 TLS 不支持 | `ClientConfig::proxy` → `Unsupported`；`tls=false` → `Unsupported` | 自定义 `ISocket` 做 CONNECT 隧道；`TcpSocket` 支持 ws:// |

### P1：影响体验/扩展性

| 问题 | 建议 |
| --- | --- |
| CMake 依赖未按 target 裁剪 | 只构建 codec 时不应强制 OpenSSL/ZLIB/Python；用 `if(PB2027_BUILD_CLIENT)` 等条件查找 |
| `acquire_token` 无连接复用 | 批量发 token 时加一个内部 HTTP client/连接池，或提供批量 API |
| 观战无播放器 | `Board` 只支持立即应用；增加按 `public_period_ms` 摊开的 playback 层 |
| 单一 `on_tick` 回调 | `Client` 支持多监听器，调度器/用户逻辑可并存 |
| 速率/重试 helper | 提供 `next_send_delay()` / 自动 `retry_after` 处理，减少调用方样板 |

### P2：锦上添花

- `Board` 不校验 `PUBLIC.from` 与当前 `version()` 的连续性（可加 `strict_continuity` 选项）。
- `/admin/ws` 未实现（管理员实时作者/IP）。
- Windows（POSIX blocking socket）与 32 位平台。
- ASan/UBSan/TSan CI、libFuzzer 解码 harness（本机缺 `libasan`，已用 Release + 严格单测覆盖）。
- 渲染/GUI/回放导出（当前只提供 `pixels()` span）。

---

## 10. 版本与兼容性

- 当前 `0.1.0`，M0–M6 完成；协议版本 `1`。
- 同一 minor 版本内保证源码兼容；不承诺 ABI 稳定。
- 公共头不包含第三方类型；升级 OpenSSL/zstd/httplib 不应影响用户代码。
- 协议实现以 `../protocol.md` 为准；与真机不一致处已在 §7 记录。

---

## 附录 A：结果码

| 值 | 名称 | 来源 | `value` |
| --- | --- | --- | --- |
| 0 | `Ok` | bind/unbind/paint | 0 |
| 1 | `Cooling` | paint | 剩余微秒 |
| 2 | `NoPaintToken` | paint | 0 |
| 3 | `BadPaintToken` | bind | 0 |
| 4 | `Banned` | bind/paint | 0 |
| 5 | `Closed` | paint | 0 |
| 6 | `IpLimit` | bind | 本 IP token 上限 |

## 附录 B：关闭码

| 码 | 含义 | 是否 retryable |
| --- | --- | --- |
| 1001 | 服务器关闭 | 否 |
| 1002 | 协议错误 | 否 |
| 4000 | 格式错误 | 否 |
| 4002 | 消息过大 | 否 |
| 4003 | 下行积压 | 是 |
| 4004 | 心跳超时 | 是 |
| 4005 | IP 封禁 | 否 |
