# pb2027 C++ SDK 设计计划

> 状态：草案 v0.3（已吸收外部评审；M4 Painter 已移出 SDK）
> 日期：2026-11-16
> 协议依据：`../protocol.md`，SHA-256 `25ccf58a5eb4bbc9de03b84c128ea0405a4302fa67b0373fbab19111ba6ce96a`（50969 字节）
> 评审输入：`../suggestions-gpt-6.1-sol.md`、`../basicpainter.ts`
> 架构修订：多 token 调度器已移到独立仓库 `pb2027-cpp-client`，下文 §5.6 与 M4 为历史设计记录。
> 变更记录：见 §13；v0.1 → v0.2 的主要改动集中在请求生命周期、断线语义、`poll()` 契约、`Board` 尺寸模型与 HTTP 返回建模。

---

## 给评审者的提示

这份文档是**设计计划**，不是最终 API 文档。v0.2 已针对上一轮评审重写请求生命周期与断线语义；请重点复核以下已修改处是否还有漏洞，不必在措辞上花时间：

1. `outbound_queue` / `inflight` 两级队列与 `submit()->SubmissionId` 能否覆盖「同一 tick 多 BATCH、整条被拒、写一半失败」。
2. `unsent` / `ambiguous` 的区分是否足以支撑「bind 可重发、paint 不重发」的重连策略。
3. `poll(timeout)` 的总 deadline + 排空预算 + 显式 `reconnect()` 是否彻底消除了单线程泵被阻塞的风险。
4. `active_slots_` / `desired_slots_` 的状态机是否覆盖 `IP_LIMIT`、`lost.slot`、`max_slots` 热调、同 token 换槽位。
5. `Board` 改为「尺寸由 Snapshot 决定（Option B）」后，`/ws` 带 `since` 直达 PUBLIC 的场景是否仍然可用。
6. `fetch_board()` 把 304/Snapshot/多段 PUBLIC 建模为结构化返回，是否把切分责任放对了层。

---

## 0. 摘要

目标是用 C++23 实现一个可复用的 pb2027 客户端 SDK：既能作为「绘画机器人」的底层（连接、槽位、BATCH、冷却、结果关联、重连），也能作为「观战端」的公共画面解码库（`PUBLIC`/`SNAPSHOT` + 声纳叠加）。

形态：**静态库 + pimpl**，公共头不泄漏任何第三方类型；编解码拆成独立 target，纯函数、无网络依赖。

### 0.1 关键决策

| 编号 | 决策 | 主要备选 | 理由 |
| --- | --- | --- | --- |
| D1 | C++23 | C++20 | 上游 WebSocket 库强制 C++23，本机 GCC 16 / Clang 23 均支持 |
| D2 | 静态库 + pimpl | header-only | 公共头零依赖；编译隔离 OpenSSL/zstd；代价是不能 header-only |
| D3 | 单线程显式泵 `poll()/flush()` | 内部 I/O 线程；ASIO 协程 | 上游库非线程安全；协议是 tick 驱动；无锁、可确定性测试 |
| D4 | 事件回调 + `std::expected` | future/promise；异常 | 结果无请求 ID、由 tick 推送，回调最贴合；错误不抛 |
| D5 | 显式 `reconnect()`，`poll()` 不内建连 | 自动重连；独立 I/O 线程 | 阻塞式建连会破坏 `poll(2ms)` 的 deadline 契约；重连退避放示例循环里 |
| D6 | WebSocket 库用 `rbeeli/websocketclient-cpp` | WebSocket++、IXWebSocket、Beast | 已实测可编译；header-only；TLS 走 OpenSSL；transport 可替换 |
| D7 | HTTP 用 `cpp-httplib` | libcurl、libcpr | 本机无 libcurl 开发包且无 sudo；cpp-httplib 单头、复用 OpenSSL |
| D8 | 依赖 vendor/锁 commit | 跟随上游 main | 上游 API 可能变；协议实现需要可复现构建 |
| D9 | 不做 `/admin/ws`、渲染、验证码求解 | — | 非核心；v2 再议；captcha 只透传 |
| D10 | 命名统一 `pb2027` | — | 用户更正；目录已从 `pd2027-cpp-sdk` 改为 `pb2027-cpp-sdk` |
| D11 | `submit()` 返回 `SubmissionId`；`outbound` / `inflight` 两级队列 | 入队即进结果 FIFO | 结果按连接内记录顺序返回；只有写出 socket 后才能进 FIFO |
| D12 | `Board` 尺寸由 `Snapshot` 决定（Option B），`reset()` 可用 `RULES` 预置 | 构造时固定尺寸 | `since` 命中保留批次时可能先收到 `PUBLIC`；`/api/board` 也可能先给 Snapshot |
| D13 | `/api/board` 在 API 层完成解压、切分、解码，返回结构化结果 | 只返回原始 payload | 304 / Snapshot / 多段 PUBLIC 必须可区分 |

### 0.2 非目标

- 不做像素画渲染、窗口、GUI。
- 不做验证码识别、不访问洛谷（只透传 `captcha`/`paste`/`access_key`）。
- 不实现 `/admin/ws`（管理员实时流），留 v2。
- 不追求 ABI 稳定；只保证同一 minor 版本内源码兼容。
- 不支持 32 位平台；首版只保证 Linux（上游 blocking transport 基于 POSIX）。

---

## 1. 目标与成功标准

### 1.1 目标

1. 给定 host + 一个有效 paint token，能在 10 行代码内开始落笔并收到结果。
2. 提供冷却感知的调度器，使单 token 的长期产率接近协议上限（邻近目标约 1 笔 / (`c0` + `k·d`)）。
3. 断线后能显式重连、自动重绑期望槽位、续传公共版本，不丢槽位语义、不误用失效 token。
4. 编解码可离线单测：给定字节序列，输出可预测的结构；可被 Python 独立实现交叉验证。
5. 观战端能把 `PUBLIC`/`SNAPSHOT` 应用到一块可查询的公共画面，并正确叠加声纳。

### 1.2 成功标准（可验收）

- M1 的 codec 单测与 Python 参考实现逐字节一致。
- M2 的 mock server 覆盖：RULES、结果顺序、`lost.slot`、Ping/Pong、4000/4002/4003 关闭、重连 `since`。
- M3 的断线测试能区分 `unsent` 与 `ambiguous`，且 `paint` 不会被自动重发；`bind` 重发幂等。
- M3 的模拟测试中，单位时间有效落笔数不低于理论值的 90%（给定目标分布）。
- M5 的观战端在 32 次/秒的 `PUBLIC` 流下内存稳定、版本连续。
- ASan/UBSan 下跑完整 mock server 场景无报错。

---

## 2. 环境与约束

### 2.1 实测环境

| 项 | 值 |
| --- | --- |
| OS | Fedora Linux 45 |
| 编译器 | GCC 16.2.1 / Clang 23.1.5 |
| 构建 | CMake 4.3.0 + Ninja 1.13.2 |
| zstd | 系统 1.5.7（头文件 + 库） |
| OpenSSL | 系统 4.0.3（同时存在 3.5.9 运行库 `libssl.so.3`） |
| zlib | 系统 zlib（头文件 + 库） |
| Python | 3.15.2（标准库 `compression.zstd` 可用，用于 mock server） |
| 缺失 | Boost、standalone Asio、libcurl 开发包、libcpr、vcpkg/conan |
| 权限 | `sudo` 被 no-new-privileges 阻止，不能装系统包 |
| 网络 | 通过 `http_proxy/https_proxy=http://127.0.0.1:7897` 访问 GitHub |
| 真实服务器 | `https://pb.ika.zip/`（2026-11-17 抓取 `/api/meta`；max_slots=8000、c0=1s、k=0.1s/px、public_delay=60s） |

### 2.2 已完成的可行性验证

```bash
# 2026-11-16，临时 clone 后编译上游 blocking 示例
g++ -std=c++23 -O0 -I include examples/builtin/ex_echo_builtin.cpp \
    -lssl -lcrypto -lz -o /tmp/wscc_echo
# → exit 0，仅有 nodiscard 警告
```

结论：`websocketclient-cpp` 在 GCC 16 + OpenSSL 4.0.3 下可编译。验证用的临时目录已删除。上游没有 `v0.7.3` 这个 ref，因此锁 commit `96a3f3f`。

### 2.3 协议硬约束（对设计有影响的）

| 约束 | 来源 | 影响 |
| --- | --- | --- |
| 每条消息恰好一个 zstd frame，无字典，窗口 ≤ 8 MiB，解压 ≤ 1 MiB | §3.2 | 编解码必须校验帧边界；不能开 permessage-deflate |
| 上行只有 `BATCH`，列式布局、`pad4`、小端 | §3.3/§3.4 | 手写字节编解码，禁止结构体直接映射 |
| 结果无请求 ID，只按记录顺序 | §3.6 | SDK 必须内部维护 FIFO 并回填 |
| 一条连接最多 `max_slots`（默认 4096）个槽位；同连接内一个 token 只占一个槽位 | §3.5 | 槽位表是连接态；重连后必须重绑 |
| 冷却按 uid：`ready = t + c0 + k·d`（`d` 为曼哈顿距离） | §1.3 | 调度器必须按每 uid 的画笔算等待，不能按连接 |
| 画笔属于 uid，TICK 推送权威画笔 | §1.2/§3.8 | 客户端不需要猜；乐观更新要能回滚 |
| 服务器 10 秒 Ping、20 秒无 Pong 断开 | §3.10 | 必须手动回 Pong（上游库不自动回） |
| 下行积压 16 MiB / 10 秒 → `4003` | §3.11 | 必须持续读 socket，即使不关心内容 |
| 4000/4002 计违规，10 次/10 分钟自动封 IP | §3.11 | 客户端必须先自校验，避免发错消息 |
| 公共画面默认延迟 10 分钟，`SNAPSHOT` 可在任意时刻出现 | §1.5/§3.9 | 观战端不能假设“先快照后增量” |

> 协议里 `public.tps`、`public.retention_ms`、`public.egress_bytes_per_s` 是服务端配置，**不在** `RULES` 也不在 `/api/meta` 里；客户端只能从 `PUBLIC` 消息自带的 `p` 得知当前采样率，不能硬编码 32。

---

## 3. 依赖选型

### 3.1 选型表

| 依赖 | 版本 / 锁定 | 用途 | 获取方式 |
| --- | --- | --- | --- |
| websocketclient-cpp | commit `96a3f3f`（CHANGELOG 0.7.3，MIT） | WS 握手、帧、TLS 传输 | vendor 到 `third_party/websocketclient-cpp` |
| OpenSSL | 系统 4.0.3 | WSS/HTTPS | `find_package(OpenSSL REQUIRED)` |
| zstd | 系统 1.5.7 | 协议消息压缩 | `find_package(zstd CONFIG)` 或 pkg-config |
| zlib | 系统 | WS 库的硬依赖（permessage-deflate 代码路径） | `find_package(ZLIB REQUIRED)` |
| cpp-httplib | 0.66.0（MIT） | `/api/meta`、`/api/paint-token`、`/api/board` | vendor 单头 |
| nlohmann/json | 3.x（MIT） | 解析 meta/token 响应 | vendor 单头 |
| doctest | 2.x（MIT） | 单测 | vendor 单头 |
| Python | 3.15.2 | mock server / 参考实现 | 系统 |

### 3.2 被否方案与原因

| 方案 | 否决原因 |
| --- | --- |
| WebSocket++（zaphoyd） | 2018 年后基本停更，GCC 16 + OpenSSL 4 兼容性未知；依赖 Boost.Asio（本机无 Boost） |
| Boost.Beast | 需要完整 Boost；无 sudo、体积大、构建慢 |
| IXWebSocket | 可用备选，但多一个 CMake 依赖且 TLS 兼容性同样未验证；当前选型已验证，没必要换 |
| libwebsockets | 需要系统安装或源码构建，C API 重，收益低 |
| libcurl + libcpr | 本机无 libcurl 开发包；cpr 只是 libcurl 包装，构建链更长 |
| 手写 HTTP/1.1 | chunked、重定向、代理、超时、TLS 校验都要自己处理，不划算 |

### 3.3 可复现性策略

- 所有第三方源码放进 `third_party/`，附 `LICENSE` 与来源 commit；CMake 默认用 vendor，`PB2027_FETCH_DEPS=ON` 时才走 FetchContent。
- 记录每个依赖的 SHA-256 或 commit；`PLAN.md` 与 `third_party/README.md` 同步更新。
- 不在构建时访问网络；CI 与本地构建行为一致。

---

## 4. 架构

### 4.1 分层

```
┌─────────────────────────────────────────────────────┐
│ 应用层：example painter / 观战端 / 用户代码          │
├─────────────────────────────────────────────────────┤
│ 策略层：Painter（调度）、Board/SonarTable（观战）    │
├─────────────────────────────────────────────────────┤
│ 会话层：Client（连接、槽位、结果、重连、时钟）       │
│         api（meta / token / board）                  │
├─────────────────────────────────────────────────────┤
│ 协议层：codec（Batch 编码、消息解码）——纯函数        │
├─────────────────────────────────────────────────────┤
│ 传输层：WS 适配（websocketclient-cpp，pimpl 隐藏）   │
│         HTTP 适配（cpp-httplib，pimpl 隐藏）         │
└─────────────────────────────────────────────────────┘
```

依赖方向只能向下：`Painter → Client → codec`；`Board` 只依赖 `codec`；`api` 只依赖 HTTP 适配。`codec` 不 include 任何网络/OpenSSL 头。

### 4.2 target 划分

| target | 类型 | 依赖 | 说明 |
| --- | --- | --- | --- |
| `pb2027::codec` | 静态库 | zstd | 类型 + Batch + 编解码；可独立使用/测试 |
| `pb2027::client` | 静态库 | codec、WS 库、OpenSSL、zlib | Client（连接/槽位/重连），**不含 HTTP** |
| `pb2027::api` | 静态库（可选） | codec、cpp-httplib、OpenSSL | meta / paint-token / board；不链它也能只用 Client |
| `pb2027::painter` | 静态库 | client | Painter |
| `pb2027::viewer` | 静态库 | codec | Board / SonarTable（无网络） |
| `pb2027::tests` | 可执行 | 全部 + doctest | 单测 |
| `pb2027::mock_server` | Python 脚本 | 标准库 | 集成测试 |

### 4.3 目录结构

```
pb2027-cpp-sdk/
├── PLAN.md
├── CMakeLists.txt
├── cmake/
│   ├── pb2027Config.cmake.in
│   └── deps.cmake
├── include/pb2027/
│   ├── pb2027.hpp          # 伞头
│   ├── types.hpp
│   ├── error.hpp
│   ├── codec.hpp
│   ├── client.hpp
│   ├── painter.hpp
│   ├── board.hpp
│   └── api.hpp
├── src/
│   ├── codec.cpp
│   ├── client.cpp
│   ├── ws_transport.cpp    # 唯一 include websocketclient-cpp 的地方
│   ├── http.cpp            # 唯一 include cpp-httplib/nlohmann 的地方
│   ├── api.cpp
│   ├── painter.cpp
│   └── board.cpp
├── examples/
│   ├── painter_cli.cpp
│   └── viewer_cli.cpp
├── tests/
│   ├── test_codec.cpp
│   ├── test_client.cpp
│   ├── mock_server.py
│   └── golden/
└── third_party/
    ├── websocketclient-cpp/
    ├── httplib/
    ├── nlohmann/
    └── doctest/
```

### 4.4 硬规则

1. `include/pb2027/*.hpp` 只能 include 标准库头。
2. 只有 `src/ws_transport.cpp` 和 `src/http.cpp` 可以 include 第三方头。
3. `codec` 层不感知连接、线程、时间；所有输入都是 `span` + `Rules`/`DecodeLimits`。
4. 所有跨层错误统一为 `pb2027::Error`，不把第三方错误类型泄漏出去。

---

## 5. 公开 API

### 5.1 设计原则

- **强类型**：`PaintToken`、`SlotId`、`PixelIndex`、`Color` 不互相隐式转换，避免把 uuid 字符串、槽号、像素下标混用。
- **值语义**：回调里传 `const T&`，但 `T` 内部持有数据（`std::vector`），回调返回后仍可安全保存。
- **显式发送**：`paint()` 只入队，`flush()` 才写 socket；保证「一 tick 一条消息」由调用方掌控。
- **错误不抛**：所有可能失败的操作返回 `std::expected<T, Error>`；回调约定 `noexcept`。
- **pimpl**：`Client`、`Painter`、`Board` 都可移动、不可拷贝，析构在 `.cpp` 里，第三方类型不出现在头文件中。

### 5.2 `types.hpp`

```cpp
namespace pb2027 {

using Uid        = std::uint32_t;
using SlotId     = std::uint16_t;    // 线上 u16；max_slots > 65535 无意义
using PixelIndex = std::uint32_t;
using TickNumber = std::uint64_t;

struct SubmissionId { std::uint64_t value = 0; };

struct Color {
    std::uint32_t v;
    static constexpr Color rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
        return Color{std::uint32_t(r) | (std::uint32_t(g) << 8) |
                     (std::uint32_t(b) << 16) | 0xFF000000u};
    }
    constexpr std::uint8_t r() const noexcept { return v & 0xFF; }
    constexpr std::uint8_t g() const noexcept { return (v >> 8) & 0xFF; }
    constexpr std::uint8_t b() const noexcept { return (v >> 16) & 0xFF; }
};

struct PaintToken {
    std::array<std::byte, 16> bytes{};
    static std::optional<PaintToken> from_uuid(std::string_view) noexcept;
    std::string to_uuid() const;
};

struct Rules {
    std::uint16_t width = 0, height = 0;
    std::uint16_t sonar_radius_px = 0;
    std::uint32_t tps = 0;
    std::uint64_t cooldown_base_us = 0;
    std::uint64_t cooldown_per_px_us = 0;
    std::uint64_t public_delay_ms = 0;
    std::uint64_t public_period_ms = 0;
    std::uint64_t event_start_unix_ms = 0;
    std::uint64_t event_end_unix_ms = 0;
    std::uint32_t max_slots = 0;
    std::uint32_t max_paints_per_message = 0;
    std::uint32_t msg_rate = 0;
    std::uint32_t msg_burst = 0;
    std::uint32_t max_message_bytes = 0;
    std::uint32_t max_payload_bytes = 0;
    std::uint64_t server_time_us = 0;   // RULES 编码时刻，用于校准
};

enum class ResultCode : std::uint8_t
{ Ok = 0, Cooling = 1, NoPaintToken = 2, BadPaintToken = 3,
  Banned = 4, Closed = 5, IpLimit = 6 };

enum class RecordKind : std::uint8_t { Bind, Unbind, Paint };

struct Result {
    ResultCode code;
    std::uint32_t value;        // Cooling: 剩余微秒；IpLimit: 上限；其余 0
    RecordKind kind;
    SlotId slot;
    SubmissionId submission;    // 来自哪次 submit/flush
    std::uint32_t record;       // 该提交内的记录下标（wire 顺序）
};

struct Brush {
    std::optional<PixelIndex> idx;   // 空 = 画笔为空
    std::uint64_t t_us = 0;          // 服务器接受时刻
};

struct BrushUpdate { SlotId slot; Brush brush; };
struct Sonar {
    PixelIndex center;
    std::uint8_t radius;
    std::vector<Color> colors;       // 按文档菱形顺序；越界为 0
};

struct Tick {
    TickNumber number;
    std::vector<Result>      results;
    std::vector<BrushUpdate> brushes;
    std::vector<Sonar>       sonars;
    std::vector<SlotId>      lost;
};

enum class SnapshotCodec : std::uint8_t {
    Full = 0,              // 只有当前画面
    FullWithHistory = 1,   // 当前画面 + l1/l2
};

struct Snapshot {
    std::uint16_t width, height;
    std::uint64_t version;
    SnapshotCodec codec = SnapshotCodec::Full;
    std::vector<Color> pixels;
    std::vector<Color> l1, l2;       // 仅 codec == FullWithHistory 时非空
};

struct PublicEntry { std::uint32_t key; std::uint32_t color; }; // key: wire 键；color: 0/1/2 或 RGBA
struct PublicSegment { std::uint32_t tick; std::vector<PublicEntry> entries; };
struct PublicBatch {
    std::uint16_t sample_rate;       // 消息头 p（次/秒）
    std::uint64_t from, to;
    std::uint32_t order;             // 0 行优先，1 列优先；换算由 Board 完成（它知道 W/H）
    std::vector<PublicSegment> segments;
};

struct UnknownMessage { std::uint8_t type; };

struct ProxyConfig { std::string host; std::uint16_t port; };

} // namespace pb2027
```

### 5.3 `error.hpp`

```cpp
namespace pb2027 {

enum class ErrorKind : std::uint8_t {
    Transport, Tls, Handshake, Protocol, Timeout, Closed,
    RateLimited, Banned, InvalidArgument, Unsupported, Internal
};

struct Error {
    ErrorKind kind;
    std::string message;
    std::optional<std::uint16_t> close_code;             // 4000/4002/...
    std::optional<std::chrono::seconds> retry_after;     // 429 用
};

enum class LogLevel : std::uint8_t { Error, Warn, Info, Debug, Trace };

} // namespace pb2027
```

约定：`Error` 可复制、可格式化（提供 `to_string(const Error&)`）；不携带第三方 error_code。

### 5.4 `codec.hpp` + `Batch`

```cpp
namespace pb2027 {

struct RecordMeta {
    RecordKind kind;
    SlotId slot;
};

class Batch {
public:
    Batch& bind(SlotId, PaintToken);
    Batch& unbind(SlotId);
    Batch& paint(SlotId, PixelIndex, Color);
    std::size_t size() const noexcept;
    std::span<const RecordMeta> wire_records() const noexcept; // 绑定/解绑在前，绘画在后
    void clear() noexcept;
};
namespace codec {

inline constexpr std::uint32_t kDefaultMaxBytes = 1u << 20;

struct DecodeLimits {
    std::uint32_t max_message_bytes = kDefaultMaxBytes;
    std::uint32_t max_payload_bytes = kDefaultMaxBytes;
    std::uint32_t max_slots = 65536;
    std::uint32_t max_segments = 1u << 16;
    std::uint32_t max_entries = 1u << 20;
    std::uint32_t max_sonar_cells = 1u << 20;
    std::uint32_t max_tick_records = 1u << 20;
    // PUBLIC 不需要 W/H（保留 wire key，由 Board 换算）；
    // TICK/SNAPSHOT 用它们做像素范围校验，0 表示跳过（如解码首条 RULES 时）。
    std::uint32_t width = 0, height = 0;
};

using DecodedMessage =
    std::variant<Rules, Snapshot, PublicBatch, Tick, UnknownMessage>;

// 编码：校验 nb/np、槽位、idx、alpha，输出单个 zstd frame
std::expected<std::vector<std::byte>, Error>
encode_batch(const Batch&, const Rules&);

// 解码：输入已是单个 zstd frame；内部校验帧边界/解压上限
std::expected<DecodedMessage, Error>
decode_frame(std::span<const std::byte>, const DecodeLimits&);

// 已解压载荷，供 /api/board 拼接多条 PUBLIC 时复用
std::expected<DecodedMessage, Error>
decode_payload(std::span<const std::byte>, const DecodeLimits&);

} // namespace codec

} // namespace pb2027
```

> `DecodeLimits` 必须独立于 `Rules`，因为解码第一条 `RULES` 时还没有 `Rules`。

### 5.5 `client.hpp`

```cpp
namespace pb2027 {

enum class State : std::uint8_t { Idle, Connecting, Open, Closed };
enum class PollStatus : std::uint8_t { Ok, Timeout, Closed };

struct ClientConfig {
    std::string host;
    std::uint16_t port = 443;
    bool tls = true;
    bool verify_peer = true;
    std::string ca_file;                             // 空 = 系统 CA
    bool public_stream = false;
    std::optional<TickNumber> since;
    std::optional<ProxyConfig> proxy;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds read_timeout{1000};
    std::chrono::milliseconds write_timeout{1000};
    std::size_t max_messages_per_poll = 64;          // 防止公共流饿死 flush
    int zstd_level = 3;                              // BATCH 压缩级别
    std::function<void(LogLevel, std::string_view)> logger;
};

struct CloseInfo {
    std::uint16_t code = 0;
    std::string reason;
    bool retryable = false;        // 4005/1001 等为 false
    std::size_t unsent = 0;        // 还在 outbound、从未写出的记录数
    std::size_t ambiguous = 0;     // 已写出但未收到结果的记录数
};

class Client {
public:
    explicit Client(ClientConfig);
    ~Client();
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;

    std::expected<void, Error> connect();            // 建连 + 握手 + 等到 RULES
    std::expected<void, Error> reconnect();          // close + connect + 重绑 + since
    void close();

    void on_rules(std::function<void(const Rules&)>);
    void on_tick(std::function<void(const Tick&)>);
    void on_snapshot(std::function<void(const Snapshot&)>);
    void on_public(std::function<void(const PublicBatch&)>);
    void on_close(std::function<void(const CloseInfo&)>);
    void on_error(std::function<void(const Error&)>);

    State state() const noexcept;
    const Rules& rules() const noexcept;
    std::uint64_t server_now_us() const noexcept;    // 已用 RULES 校准
    std::optional<TickNumber> public_version() const; // 已应用到的 Vpub（重连 since 用）
    std::optional<Brush> brush(SlotId) const;         // active slot 的权威画笔
    std::optional<PaintToken> active_token(SlotId) const;
    std::optional<std::uint64_t> ready_at_us(SlotId, PixelIndex) const;
    // ready_at_us：槽位未绑定返回 nullopt；画笔为空返回 server_now_us()；否则按 §1.3 计算。
    std::size_t inflight_records() const noexcept;    // 已写出、未回结果的记录数

    // 发送：只入队
    void bind(SlotId, PaintToken);                   // 期望绑定，重连时自动恢复
    void unbind(SlotId);
    void paint(SlotId, PixelIndex, Color);
    std::expected<SubmissionId, Error> submit(Batch&&);  // 显式提交一整条 BATCH

    // 泵
    PollStatus poll(std::chrono::milliseconds timeout);  // 总 deadline，只收不发
    struct FlushResult {
        bool sent = false;
        SubmissionId id;
        std::size_t records = 0;
        std::size_t bytes = 0;
        std::optional<std::chrono::microseconds> retry_after;
    };
    FlushResult flush();                             // 至多一条 BATCH

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pb2027
```

语义细节：

- `connect()` 在收到 `RULES` 后返回；`on_rules` 同时触发一次。重复调用 `connect()` 是错误。
- `poll(timeout)` 是**整个调用的总 deadline**：先等第一条消息，然后在 deadline 和 `max_messages_per_poll` 双重预算内尽量排空；每处理一条后重新检查 deadline。处理 `PingFrame`（回 Pong）、`CloseFrame`（清理 + 回调）。**`poll()` 从不发起或阻塞在建连上**。
- 上述 deadline 约束的是“等待消息”的时间；单条消息的读取仍受 `read_timeout` 限制（上游把 `read_message` 超时视为致命错误），所以 `read_timeout` 设 1–5 s，慢网络/大消息下 `poll` 可能略超 `timeout`。
- `reconnect()` 显式阻塞执行：`close()` → `connect()` → 发重绑 BATCH → 带 `since` 恢复公共流；返回时已收到 `RULES`，但**不等重绑结果**。退避由调用方循环实现。
- 注意：上游 `read_message` 的超时按其契约属于错误（必须关闭连接），所以必须先用 `wait_message` 判断可读；`read_timeout` 不要设得太短（默认 1 s）。
- 结果关联：`bind/unbind/paint` 先进入隐式批次；`flush()` 成功写出后才把 `PendingRecord{kind, slot, submission, record}` 追加到 `inflight_`。`TICK` 的 `nr` 个结果按顺序出队并回填 `kind/slot/submission/record`。`nr > inflight_.size()` 视为协议错误并关闭连接；剩余未回的记录是正常状态。
- `bind()` 是声明式：写入 `desired_slots_`。收到 `Bind + OK` 才更新 `active_slots_`；`Unbind + OK` 清空 active；失败的 bind 不改 active。
- `BAD_PAINT_TOKEN`/`BANNED` 从 `desired_slots_` 移除并 `on_error`；`IP_LIMIT` 保留 desired 但不自动重试，等用户处理。
- `lost.slot`：清空该槽位的 `active_slots_` 与 `desired_slots_`（四种原因都是终态），不自动重绑。
- `submit()` 提交显式 `Batch`，返回 `SubmissionId`（单调递增）；空 `Batch` 返回 `InvalidArgument`；不进入 `desired_slots_`，重连后不自动重发。
- `on_snapshot`/`on_public` 只在 `public_stream=true` 时有意义；`public_version()` 由收到的 `SNAPSHOT`/`PUBLIC.to` 推进。
- `flush()` 遵守 `msg_rate/msg_burst` 令牌桶；桶空时返回 `sent=false` 和 `retry_after`，不阻塞。空 batch 直接返回；一次最多编码 `max_slots` 个绑定和 `max_paints_per_message` 条绘画，超出部分留到下一次调用。`FlushResult.id` 是本次实际发送批次的 id（可能早于某次 `submit()` 返回的 id）。
- 断线时：`outbound_` 里未写出的记为 `unsent`，`inflight_` 里已写出的记为 `ambiguous`。**paint 一律不自动重发**（可能已生效）；`bind` 通过 `desired_slots_` 在 `reconnect()` 时幂等重发。
- `close()` 发送 WS Close 帧并关闭 socket；`state` 变为 `Closed`。`reconnect()` 只用于 `Closed`/`Open` 状态。

### 5.5.1 请求生命周期

```
paint()/bind() ──> outbound_queue_ ──flush() 编码+写出──> inflight_ ──TICK.nr──> Result
                        │                                    │
                        └─ 断线：unsent（不重发 paint）       └─ 断线：ambiguous（不重发 paint）
```

- 一条 `BATCH` 的所有记录在 `flush()` 返回成功后才一起进入 `inflight_`；部分写失败视为整条 `ambiguous` 并关闭连接。
- `PendingRecord` 是内部结构：`{ RecordKind kind; SlotId slot; SubmissionId submission; std::uint32_t record; }`；`Batch::wire_records()` 只提供 `{kind, slot}`，`submission/record` 在发送时补全。
- `SubmissionId` 在 `submit()`/`flush()` 时分配；同一 `BATCH` 内记录下标 `record` 从 0 开始，按 wire 顺序（绑定/解绑在前，绘画在后）。
- `Batch::wire_records()` 的 wire 顺序必须与编码顺序一致；`Client` 内部另存 `RecordMeta`，不依赖用户再次解析。
- 顺序保证：`bind/unbind/paint` 先累积在隐式批次里；`submit()` 会**先把隐式批次入队**，再入队显式 `Batch`，因此“先 bind 后 submit paint”不会乱序。`flush()` 只发送队首一条；隐式批次在 `flush()` 时分配 `SubmissionId`，通过 `FlushResult.id` 暴露。
- 非 `Open` 状态调用 `submit()` 返回 `Error{Closed}`；`paint()` 被丢弃并 `on_error`；`bind/unbind` 只更新期望表。

### 5.6 `painter.hpp`

```cpp
namespace pb2027 {

struct Target {
    PixelIndex idx;
    Color color;
    std::uint8_t priority = 0;       // 高者优先
    bool allow_same_color = false;   // 允许同色重涂（侦察/占位）
};

struct PainterOptions {
    std::size_t max_paints_per_tick = 512;
    bool honor_cooling_value = true; // 用 COOLING 微秒值精确等待
    bool probe_with_sonar = false;
    std::chrono::milliseconds probe_interval{0};
};

class Painter {
public:
    explicit Painter(Client&, PainterOptions = {});
    ~Painter();
    Painter(Painter&&) noexcept;

    void add_token(SlotId, PaintToken);
    void remove_token(SlotId);

    void set_targets(std::vector<Target>);
    void add_target(Target);
    void clear_targets();

    void tick();                     // 每循环一次：取目标 → 入队（flush 由调用方执行）
    struct Metrics {
        std::uint64_t strokes_ok = 0, strokes_cooling = 0, strokes_rejected = 0;
        double avg_distance = 0.0;
        std::size_t tokens_active = 0;
    };
    Metrics metrics() const;

private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace pb2027
```

调度策略（v1）：

0. `add_token` 内部调用 `Client::bind()`，槽位进入期望表，重连后自动恢复。
1. 每个 token 维护 `brush` 与 `ready_at`（来自 TICK / `ready_at_us`）；**不做乐观画笔更新**，服务器 TICK 是唯一权威。
2. 目标按 32×32 网格分桶；对每个就绪 token，从所在格向外扩环搜索最近且颜色不同的目标（通常 3×3 内命中；扩到上限仍无则退化为全局扫描）。
3. 若最近目标未就绪，按 `COOLING` 的 `value` 记下等待，不盲目重试；画笔为空时不受位置限制，直接取最高优先级目标。
4. 每次决定发送前**重新读取** `brush(slot)` 和 `server_now_us()`，不使用上一次循环缓存的 `ready_at`（别的连接或同 uid 的笔可能已经移动了画笔）。
5. 每 tick 最多 `max_paints_per_tick` 条记录，按 token 轮转，避免一个 token 吃满；同一 token 因冷却每 tick 最多一条。
6. `probe_with_sonar` 开启时，空闲 token 按 `probe_interval` 原地重涂以维持声纳。

> `basicpainter.ts` 的单 token 版本验证了上述主流程；C++ 版在此基础上补多槽位、`lost.slot`、结果 FIFO 与重连重绑。

### 5.7 `board.hpp`

```cpp
namespace pb2027 {

class Board {
public:
    Board();                         // 未初始化；reset() 或 apply(Snapshot) 决定尺寸
    ~Board();
    Board(Board&&) noexcept;

    void reset(std::uint16_t width, std::uint16_t height);  // 从 RULES/meta 预置尺寸（/ws?since 路径）
    std::expected<void, Error> apply(const Snapshot&);     // 初始化 / 按快照尺寸重建，并设置历史层
    std::expected<void, Error> apply(const PublicBatch&);  // 用自身 W/H 把 wire key 换算成 idx
    void overlay(const Sonar&, TickNumber);   // 声纳优先显示
    void prune(TickNumber public_version);    // 过期声纳清理

    bool initialized() const noexcept;
    std::uint16_t width() const noexcept;
    std::uint16_t height() const noexcept;
    TickNumber version() const noexcept;
    std::optional<Color> color_at(PixelIndex) const;
    std::optional<std::pair<Color, TickNumber>> sonar_at(PixelIndex) const;
    std::span<const Color> pixels() const noexcept;  // 尺寸变化后失效

private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

class SonarTable {
public:
    SonarTable();                                  // 0x0，add() 忽略
    SonarTable(std::uint16_t width, std::uint16_t height);
    ~SonarTable();
    SonarTable(SonarTable&&) noexcept;

    void reset(std::uint16_t width, std::uint16_t height);
    void add(const Sonar&, TickNumber);            // 需要 W/H 换算菱形偏移
    void prune(TickNumber);
    std::optional<std::pair<Color, TickNumber>> at(PixelIndex) const;

private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace pb2027
```

渲染规则（§3.9）：声纳表 `K` 的项优先于公共画面；`Vpub >= t` 时该项过期。`Board` 内部用「颜色数组 + tick 数组 + FIFO」实现，避免每次全表扫描。

实现补充（M5 落地时的取舍）：

- `SonarTable::add(Sonar)` 必须知道 W/H 才能把菱形偏移换算成 idx；因此提供 `SonarTable(w,h)` 与 `reset(w,h)`，默认 0×0 时忽略 add。
- `Board::apply(PublicBatch)` 在只有 `reset()` / codec `Full` 快照（无历史层）时，遇到颜色码 0/1 返回 `InvalidArgument`；纯直接颜色仍可应用。
- `Board` 对 PUBLIC 做整批预校验（段序、`from+seg.tick <= to`、order、alpha），失败时保持原状。
- `SnapshotCodec::FullWithHistory` 下载入时把 l1/l2 的 0（“空”）归一化为当前像素色，匹配 §3.7.3 的“空 == 当前色”模型。

尺寸模型采用 **Option B + reset()**：`Board` 默认未初始化；`apply(Snapshot)` 是初始化/重建入口，`reset(w,h)` 允许在收到 `PUBLIC` 前用 `RULES`/`/api/meta` 的宽高预置尺寸（`/ws?since` 命中保留批次时会直达 `PUBLIC`），并清空画面、历史层与声纳。`apply(PublicBatch)` 在未初始化时返回 `Error`（`InvalidArgument`）。`PublicBatch` 保留 wire key，由 `Board` 用自身 W/H 换算 idx（`order=1` 需要 H，这样 codec 不必依赖画布尺寸）。

收到 `Snapshot` 重建时清空声纳表，并让此前 `pixels()` 返回的 span 失效——该 span 只在两次 `apply` 之间有效。`SnapshotCodec::Full` 时 `l1/l2` 为空，`FullWithHistory` 时两者必须恰好 `W·H` 个。若 `Board` 只有 `reset()` 而没有 `Snapshot`，历史层为空，此时不能正确解释 `PUBLIC` 里的颜色码 0/1；这条路径要求调用方先拿到过同版本的 `Snapshot`。

### 5.8 `api.hpp`

```cpp
namespace pb2027::api {

struct Endpoint {
    std::string host;
    std::uint16_t port = 443;
    bool tls = true;
    bool verify_peer = true;
    std::string ca_file;
    std::string base_path = "/";                  // 非根路径部署
    std::optional<ProxyConfig> proxy;
    std::optional<std::string> proxy_username, proxy_password;
    std::chrono::milliseconds request_timeout{10000};
};

struct Meta {
    std::uint32_t protocol;
    Rules rules;     // /api/meta 的 rules + limits 合并进 Rules
    std::uint16_t width, height;  // 来自 /api/meta 的 board
    std::string captcha_provider, captcha_site_key, captcha_api_endpoint;
    std::string access_mode, luogu_paste_text;
    std::uint64_t public_version;
    std::uint64_t server_time_ms;
};

struct IssuedToken {
    Uid uid;
    PaintToken token;
    std::optional<std::string> access_key;
};

struct AccessKeyRequest { std::string access_key; };
struct CaptchaRequest { std::string captcha; std::string paste; };
struct UidRequest { Uid uid; };  // 测试模式（access_mode == "none"）

struct BoardResponse {
    enum class Kind : std::uint8_t { NotModified, Snapshot, Public };
    Kind kind;
    std::uint64_t version;
    std::optional<Snapshot> snapshot;   // kind == Snapshot
    std::vector<PublicBatch> batches;   // kind == Public；可能 0 段
};

std::expected<Meta, Error> fetch_meta(const Endpoint&);
std::expected<IssuedToken, Error> acquire_token(
    const Endpoint&, std::variant<AccessKeyRequest, CaptchaRequest, UidRequest>);
std::expected<BoardResponse, Error> fetch_board(
    const Endpoint&, std::optional<std::uint64_t> since);

} // namespace pb2027::api
```

`api` 是**可选模块**：核心绘画链路只需要一个现成 token，不链 `pb2027::api` 也能用 `Client`。这样 HTTP 库永远不在关键路径上。

实现备注：`fetch_meta` 会把 `/api/meta` 的 `board.width/height` 同时填进 `Rules`，并用 `server_time_ms*1000` 填 `Rules.server_time_us`，方便直接复用；`/api/board` 的解码上限放宽到 64 MiB（真机 codec-1 快照 7.2 MB）。非 200 除 429 外暂统一映射为 `Protocol`。

`fetch_board` 在 API 层完成：HTTP 请求 → 读 `X-Board-Version`/`ETag` → zstd 解压 → 按首字节区分 `0x82`/`0x83` → 多条 `PUBLIC` 按 `36 + 8m + pad4(n) + 4e + 4n` 切分 → `codec::decode_payload`。`304` 返回 `Kind::NotModified`，不以错误表示；`429` 返回 `Error{RateLimited, retry_after}`。调用方不需要自己切分首尾相接的 payload。**若调用方没有历史层（没拿到过同版本 `Snapshot`），不要带 `since`**：增量里的颜色码 0/1 需要 `l1/l2` 才能解释，此时应请求全图快照。

### 5.9 线程、生命周期、回调契约

1. **线程亲和**：所有方法必须在同一线程调用（创建 `Client` 的线程）；`poll()` 内派发回调。
2. **回调禁忌**：回调里可以 `paint()`/`bind()`/`submit()`（入队），不要再次调用 `poll()`/`flush()`/`reconnect()`；回调不得抛出异常。
3. **参数生命周期**：回调参数仅在回调期间有效；`Tick`/`PublicBatch` 等值类型内部持有数据，但建议在回调内消费完，需要留存时自行拷贝。
4. **销毁顺序**：`Painter` 必须先于 `Client` 析构；`Board` 独立。
5. **线程安全**：不提供任何跨线程接口；需要跨线程时，调用方自己把命令投递到泵线程。
6. **泵预算**：`poll()` 的总耗时不超过 `timeout`，且单次最多处理 `max_messages_per_poll` 条消息；不会因为公共流积压而无限推迟 `flush()`。
7. **重连责任**：SDK 不在 `poll()` 内建连；调用方在 `Closed` 后按退避调用 `reconnect()`，并用 `CloseInfo::retryable` 决定是否重试。

### 5.10 取舍说明

- **为什么不用 future/promise**：协议结果没有请求 ID，且结果由服务器按 tick 推送；future 会把「哪条结果对应哪条请求」的责任藏进库，出错时难调试。`SubmissionId + record` 是协议的真实语义。
- **为什么不内置 I/O 线程**：上游库无隐藏同步；单线程泵让「收到 TICK → 调度 → flush」在一个确定点发生，便于对齐 tick 和复现问题。
- **为什么显式 `reconnect()`**：底层建连是阻塞的；把它塞进 `poll(2ms)` 会让主循环停顿 5 秒以上，期间不能回 Pong、不能读下行。把建连交还调用方，`poll()` 的 deadline 契约才是真的。
- **为什么 `outbound` / `inflight` 两级队列**：没写出去的请求不能占结果 FIFO；写失败/断线时只有 `inflight` 是「可能已生效」的，必须和「肯定没发」的分开计数，才能决定 bind 可重发、paint 不重发。
- **为什么 `Board` 用 Option B（尺寸由 Snapshot 决定）**：`/ws` 带 `since` 命中保留批次时会先给 `PUBLIC` 而不是 `SNAPSHOT`，`/api/board` 也可能首包是 Snapshot；把尺寸交给 Snapshot 可以避免「构造时还不知道宽高」的鸡生蛋问题。
- **为什么 pimpl 而不是 header-only**：`websocketclient-cpp` 是模板库，直接暴露会导致用户每个 TU 都编译 OpenSSL/模板；pimpl 把编译单元收敛到 `src/`，公共头只依赖标准库。
- **为什么 `flush()` 不自动按 tick 发送**：调用方最清楚 tick 边界；自动发送会让「一 tick 一条」不可控，也违反 `msg_rate` 预算。

---

## 6. 协议实现清单

### 6.1 消息类型

| 类型 | 名称 | 方向 | 编解码目标 |
| --- | --- | --- | --- |
| `0x01` | `BATCH` | C→S | `Batch` → `encode_batch`（编码） |
| `0x84` | `RULES` | S→C | `Rules` |
| `0x83` | `SNAPSHOT` | S→C | `Snapshot` |
| `0x82` | `PUBLIC` | S→C | `PublicBatch` |
| `0x81` | `TICK` | S→C | `Tick` |
| 其他 | — | — | `UnknownMessage`（忽略，不报错） |

### 6.2 `BATCH` 编码

- 头 16 字节：类型 `0x01`、版本 `1`、保留 0、`nb`、`np`、保留 0。
- 列顺序：`bind.slot`（u16, pad4）→ `bind.paint_token`（16B）→ `paint.slot`（u16, pad4）→ `paint.idx`（u32）→ `paint.color`（u32）。
- 长度公式：`16 + pad4(2·nb) + 16·nb + pad4(2·np) + 8·np`。
- 记录的 wire 顺序固定为：所有绑定/解绑，再所有绘画。`Batch::wire_records()` 必须按该顺序返回；`Client` 用它建立 `RecordMeta`，不再次解析编码结果。
- 发送前自校验：`nb ≤ max_slots`、`np ≤ max_paints_per_message`、槽位 `< max_slots`、`idx < W·H`、颜色 `A = 255`。
- 空 batch 不发送；`flush()` 直接返回 `sent=false`。
- 压缩：`ZSTD_compress2`，单帧、无字典、窗口 ≤ 8 MiB、内容大小可带；默认级别 3（1–19，建议 ≤ 5）。**不要**开 permessage-deflate。
- 解绑：`bind(slot, 16 个 0)`；SDK 的 `Batch::unbind` 负责构造。
- 写 socket 使用 `write_timeout`；写失败视为整条 `ambiguous`，关闭连接，不重发 paint。

### 6.3 `TICK` 解码与结果关联

- 头 32 字节：类型、版本、`R`、`nr`、`tick`、`nb`、`nk`、`ns`、保留。
- 列：`brush.t`（u64，偏移 32，8 字节对齐）→ `brush.idx`（u32）→ `brush.slot`（u16, pad4）→ `result.status`（u8, pad4）→ `result.value`（u32）→ `sonar.idx`（u32）→ `sonar.color`（u32 × nk·A）→ `lost.slot`（u16, pad4）。
- `A = 2R² + 2R + 1`，`R ≤ 16`；`sonar.color` 按菱形顺序。
- **关联规则**：结果与 `inflight_` 的记录严格按顺序 1:1。先检查 `nr ≤ inflight_.size()`，否则协议错误并关闭连接；然后弹出前 `nr` 条，回填 `kind/slot/submission/record`。`nr` 小于 `inflight_.size()` 是正常的（记录可以留到后续 tick）。
- **状态应用顺序**（§3.8、§5 建议）：① 按结果顺序应用 bind/unbind：只有 `Bind + OK` 才写 `active_slots_`，`Unbind + OK` 才清空；失败不改表。② 清空 `lost.slot` 对应的 `active_slots_` 与 `desired_slots_`。③ 最后应用 brush 条目。
- **同 token 换槽位**：`bind(s, T)` 时若 `T` 已在 `desired_slots_` 的另一个槽位 `s'`，先删除 `s'` 的期望；`Bind + OK` 后同样从 `active_slots_` 移除 `s'`（同连接内一个 token 只占一个槽位）。
- 画笔条目只对“本 tick 截止点仍然有效”的槽位有意义；已发出但未收到结果的 bind 不算进槽位表。`brush.t = 0` 表示画笔为空；`brush.idx` 此时为 0。
- `brush.slot`/`lost.slot` 严格升序；`TICK` 不合并、不丢弃。

### 6.4 `PUBLIC` / `SNAPSHOT`

- `SNAPSHOT` 可能在任意时刻出现，不能假设只在开头（§3.9）。
- `PUBLIC` 头 36 字节；列：`seg.tick`（u32×m）→ `seg.n`（u32×m）→ `dsmall`（u8×n, pad4）→ `dbig`（u32×e）→ `color`（u32×n）。
- 键换算：`order=0` 时 `key=i`；`order=1` 时 `i = (k mod H)·W + ⌊k/H⌋`。**codec 保留 wire key**，由 `Board::apply` 用自身 W/H 换算（`order=1` 需要 H）；`Board` 未初始化或 key 越界时返回 `Error`；`order` 只允许 0/1。
- 差值解码：`dsmall` 中 255 用 `dbig` 替换；每段独立前缀和。
- 颜色语义：0 → `l1[i]`，1 → `l2[i]`，2 → 不变（原地重绘），其他 → 新颜色。
- 历史层更新严格按 §3.7.3 的伪代码；「颜色不变」的条目不动历史层。
- **checked arithmetic**：`36 + 8m + pad4(n) + 4e + 4n` 先做溢出检查；`from < to`；`seg.tick` 严格递增；`Σ seg.n == n`；`dbig` 数量 == `dsmall` 中 255 的个数；每段前缀和（以及 `from + seg.tick`）不溢出；`m/n/e` 不超过 `DecodeLimits` 的上限；key 在段内严格递增。idx 范围检查在 `Board` 换算后做。
- `/api/board` 的解压结果可能是**多条 `PUBLIC` 首尾相接**（§2.4），由 `api::fetch_board` 按 `36 + 8m + pad4(n) + 4e + 4n` 切分后交给 `codec::decode_payload`（§5.8）。
- `Board` 需按段应用，支持播放器按 `public_period_ms` 摊开（v2 可加）。

### 6.5 `RULES` 与时钟

- `RULES` 定长 96 字节；连接后第一条消息；参数热调后会再发（§3.7）。
- 校时：记录 `server_time_us` 与本地 `steady_clock` 的差值；`server_now_us()` 用它换算。
- `RULES` 不含 `public.tps`，采样率只能从 `PUBLIC.p` 读。

### 6.6 心跳、关闭、限流

- 收到 `PingFrame` → `send_pong_frame(payload)`。**实测确认**：`websocketclient-cpp` 明确“不隐藏控制流”，示例也手动回 Pong；若后续版本改为自动回，适配层必须避免重复。mock server 要覆盖“发送大 BATCH 期间 Ping 仍及时 Pong”。
- 收到 `CloseFrame` → 记录 code/reason → `close()` → 触发 `on_close`；`retryable` 由关闭码决定：4000/4002/4005/1001 为 false，4003/4004/传输错误为 true；调用方据此决定是否 `reconnect()`。
- 发送侧令牌桶：`msg_rate/msg_burst`；记录侧不主动限制（服务器会排队），但 SDK 可根据 `msg_rate` 给调度器提示。
- 下行必须持续读：`poll()` 在 deadline 内排空；即使不关心 `TICK` 也要读掉，避免 16 MiB / 10 秒积压（`4003`）。
- 写阻塞：blocking transport 的写有 `write_timeout`；BATCH 应保持在数百 KB 以内，避免单次写占用过久导致 Pong/读下行延迟。

### 6.7 重连状态机

```
Idle --connect()--> Connecting --RULES--> Open
Open --close/error--> Closed
Closed --reconnect()（调用方先退避）--> Connecting --RULES--> Open
Open --user close()--> Closed
```

`reconnect()` 成功后的动作（顺序固定）：
1. 重新 `bind` `desired_slots_`（超过单条 `max_slots`/1 MiB 上限时分多条）。
2. `since` 设为最后 `Vpub`（`public_stream` 开时）。
3. 声纳表 `K` 保留（与连接无关，§3.9）。
4. `on_rules` 重新触发（参数可能变了）；按新 `max_slots` 裁剪 `desired_slots_`。
5. `BAD_PAINT_TOKEN`/`BANNED` 的槽位从期望表移除并 `on_error`；`IP_LIMIT` 保留期望但不自动重试。
6. `outbound_` 里的 paint 全部丢弃（`unsent`），`inflight_` 里的全部标记 `ambiguous`；两者都不自动重发，交给调度器重新决策。

断线语义：

| 类别 | 定义 | 重连策略 |
| --- | --- | --- |
| `unsent` | 还在 `outbound_`，从未写出 | bind/unbind 由期望表重建；paint 丢弃 |
| `ambiguous` | 已写出，未收到结果 | 不重发；paint 可能已生效，重发会多花冷却 |
| `settled` | 已收到结果 | 无需处理 |

### 6.8 易错点清单

| 点 | 错误做法 | 正确做法 |
| --- | --- | --- |
| zstd 帧 | 用流式压缩产生多帧 | 一次性 `ZSTD_compress2`，校验单帧 |
| 结果关联 | 按槽位匹配 | 严格按发送顺序 FIFO |
| 画笔 | 用本地估算覆盖服务器值 | 以 TICK 推送为准；乐观更新要记录回滚值 |
| 槽位 | 重连后沿用旧槽位表 | 新连接槽位全空，必须重绑 |
| 冷却 | 按 token 或连接算 | 按 uid 算 |
| Pong | 依赖库自动回 | 自己回 |
| PUBLIC | 假设快照只在开头 | 任意时刻可替换 |
| 关闭码 | 一律立即重连 | 4005 停止；4000/4002 先修 bug |
| 消息大小 | 不检查 | 发送前按 `max_message_bytes` 校验 |

---

## 7. 关键流程

### 7.1 连接建立

```
connect()
  → DNS + TCP（或经代理 CONNECT）
  → TLS（SNI + 证书校验）
  → WS 握手（ws[s]://host/ws?public=&since=）
  → 读 RULES → rules_ 赋值、校时
  → state = Open
  → 若有 desired_slots_：发送绑定 BATCH
```

### 7.2 主循环（调用方视角）

```cpp
pb2027::Client client(cfg);
pb2027::CloseInfo last_close{};
client.on_tick([&](const pb2027::Tick& t) { /* 更新画笔/声纳 */ });
client.on_close([&](const pb2027::CloseInfo& info) { last_close = info; });
if (!client.connect()) return 1;
client.bind(0, token);

const auto tick_period = std::chrono::microseconds(1'000'000 / client.rules().tps);
auto next = std::chrono::steady_clock::now();

while (running) {
    if (client.state() == pb2027::State::Closed) {
        if (!last_close.retryable) break;
        std::this_thread::sleep_for(backoff.next());   // 调用方负责退避
        if (auto r = client.reconnect(); !r) break;    // 失败按策略重试或退出
        next = std::chrono::steady_clock::now();
        continue;
    }
    client.poll(std::chrono::milliseconds(2));         // 只收：排空 + 回 Pong
    painter.tick();                                    // 目标 → client.paint() 入队
    client.flush();                                    // 至多一条 BATCH
    next += tick_period;
    std::this_thread::sleep_until(next);
}
```

`poll()` 返回 `Closed` 或 `on_close` 触发后，循环进入重连分支；`CloseInfo::retryable == false`（4005、1001）直接退出。

### 7.3 重连

- 退避由调用方实现：`min(initial × 2^n, max)` 加 `±jitter`；成功后重置 `n`。
- `CloseInfo::retryable == false`（4005 IP 被封、1001 服务器关闭）不再重试。
- `reconnect()` 内部顺序：`close()` → `connect()`（阻塞到 `RULES`）→ 按 `desired_slots_` 发重绑 → 带 `since` 恢复公共流；返回时不等重绑结果。重绑 BATCH 先入队，后续 paint 的 BATCH 排在它后面，服务器按消息到达顺序先处理绑定。
- 断线期间的 `bind/unbind` 只更新期望表，重连后统一生效；`paint()` 在非 `Open` 状态被丢弃并 `on_error`（调度器应停止），避免无限堆积无主请求。
- 公共声纳表 `K` 跨连接保留；`Board` 的公共画面版本也由 `public_version()` 续上。

### 7.4 冷却调度

对每个槽位 `s`：

```
brush = client.brush(s)
if (!brush) ready = now
else ready = brush.t_us + c0 + k * dist(brush.idx, target)
if (now >= ready) paint
else 记录冷却，换目标或等待
```

`COOLING` 的返回值只针对该目标位置，换更近的目标可能已就绪——调度器必须允许“换目标重试”，而不是死等。

---

## 8. 里程碑与验收

| 阶段 | 内容 | 验收标准 | 估时 |
| --- | --- | --- | --- |
| **M0** 工程骨架 ✅ | CMake、vendor 依赖、pimpl 空壳、codec/transport 接口；离线可构建 | 断网下 `configure + build` 通过；本地 `wss` echo 通过（见 PROGRESS.md） | 1d |
| **M1** codec ✅ | `Batch`/`RecordMeta`、`encode_batch`、`decode_frame`、golden vectors、Python 对照、fuzz | 15 个 test case / 115 断言通过；截断/pad/帧边界/溢出已覆盖 | 2–3d |
| **M2** 传输与会话 | WebSocket 适配、RULES、Ping/Pong、Close、TICK、`outbound`/`inflight` FIFO | mock server 正常路径 + 结果顺序 + 关闭码 | 2–3d |
| **M3** 状态机与重连 ✅（基础） | `active`/`desired` 槽位、`unsent`/`ambiguous`、`reconnect()`、`since`、热调 `max_slots` | 断线/重连/4000/`lost.slot` 测试通过；paint 不重发、bind 幂等 | 2–3d |
| **M4** 调度 ✅ | `Painter`、多 token、冷却重读、网格搜索、metrics | 冷却门控 + 最近邻测试通过；更大规模仿真待补 | 2d |
| **M5** 观战与 HTTP ✅ | `Board`、`SonarTable`、Snapshot/PUBLIC/历史层、`/api/board` 切分、304 | 21 个 Board + 10 个 API 测试通过；真机 SNAPSHOT/PUBLIC 已应用 | 2–3d |
| **M6** 交付与联调 ✅ | install/export、examples、README、sanitizer CI、真实服务器联调 | `find_package(pb2027)` 通过；真机只读 + 100 token 落笔/冷却/重连通过 | 1–2d |

合计约 **12–17 人日**；真实服务器联调放在 M6，作为风险项而不是早期硬门槛。

---

## 9. 测试策略

### 9.1 单元测试

- `Batch` 编码：空 batch、单绑定、多绑定、解绑、边界值（slot=0/max-1）、非法 alpha；与 `basicpainter.ts` 的 `encodeBatch` 对照**解压后的 payload**（zstd 压缩帧不唯一，不要直接比压缩字节）。
- 解码：每个消息类型的最小/最大合法载荷；截断、pad 非零、长度不符、越界 idx、alpha≠255、多帧、字典帧、窗口超限。
- `PUBLIC`：`order=0/1`、`dsmall` 多个 255、`dbig` 数量不符、`Σ seg.n != n`、`from >= to`、`seg.tick` 非递增、前缀和溢出；`Board` 的 key→idx 换算（含未初始化路径）。
- 历史层：构造 3 次颜色变化，验证 `l1/l2` 与 §3.7.3 伪代码一致。
- 冷却/时钟：`d = 0/1/大值` 的 `ready` 计算；`COOLING` 值封顶 `2^32−1`；UUID 文本与 16 字节互转。
- 声纳菱形顺序：`R=0`、`R=2`（默认）、`R=16`、画布四角/边缘、越界补 0；可直接用 `basicpainter.ts` 的 `diamond()` 作为对照。

### 9.2 交叉验证

- Python 按文档独立实现 `encode_batch`/`decode`，与 C++ 互测；避免“自己验证自己”。
- golden 文件：`tests/golden/*.bin` 存**解压后的 payload** + 对应解释的 JSON；压缩帧只校验 `decode_frame` 能接受且解压结果一致。

### 9.3 mock server（Python 标准库）

场景：
1. 正常握手 + RULES + 每 tick TICK。
2. 结果顺序（bind/paint 混合）；一个 BATCH 同时含 bind/unbind/paint。
3. 多个 BATCH 在同一个 TICK 返回；`nr` 小于/等于 `inflight`。
4. `COOLING`、`NO_PAINT_TOKEN`、`BAD_PAINT_TOKEN`、`IP_LIMIT`。
5. `lost.slot`（token 重领/封禁/同 token 换槽位/`max_slots` 热调）。
6. Ping/Pong 超时（4004）；发送大 BATCH 期间 Ping。
7. 4000 格式错误、4002 过大、4003 积压。
8. 重连：已写未回（ambiguous）、未写（unsent）、重连重绑、`since`。
9. `SNAPSHOT` 中途插入；PUBLIC 在 Snapshot 后/中间。
10. 协议违例：`nr > inflight`、`TICK` 结果数异常。

### 9.4 真机联调

- 使用专用 uid/token，先 `public=1` 观战，再小批量落笔。
- 控制频率，避免触发 IP 级自动封禁；记录服务器时间偏移。
- 对比服务器行为与文档不一致处，记录 issue。

### 9.5 其他

- ASan/UBSan/TSan（TSan 预期无竞争，因为单线程）。
- 解码 fuzz：libFuzzer，输入 1 MiB 以内任意字节，不得崩溃/越界。
- 性能：1 万条 painting 的 `BATCH` 编码 + 压缩 < 5 ms；单 tick 调度 < 1 ms。

---

## 10. 风险

| 风险 | 概率 | 影响 | 缓解 |
| --- | --- | --- | --- |
| OpenSSL 4 与 WS 库后续更新不兼容 | 低 | 高 | 已锁 commit + 已验证；保留 OpenSSL 3.5.9 运行库；必要时打小补丁 |
| `PUBLIC` 差分解码实现错误 | 中 | 高 | 独立模块 + Python 对照 + golden；M5 前不阻塞绘画链路 |
| 代理环境下 WS 直连失败 | 中 | 中 | `ProxyConfig` + 内部 CONNECT transport；先确认 host 是否可直连 |
| 调用方忘记退避/重连 | 中 | 中 | 示例给出标准循环；`CloseInfo.retryable` + `unsent/ambiguous` 可观测 |
| 阻塞写导致 Ping/下行延迟 | 中 | 中 | 限制单条 BATCH 在数百 KB；`write_timeout`；mock 测大 BATCH 期间的 Ping |
| 真机测试触发限流/封禁 | 中 | 高 | 专用 token、低频率、先观战；错误码区分 4000/4002/4005 |
| 上游 API 变动 | 中 | 低 | vendor + 锁 commit；隔离在 `ws_transport.cpp` |
| 结果 FIFO 在异常路径错位 | 低 | 高 | `nr ≤ inflight` 强校验；单测覆盖断线/整条拒绝；`unsent/ambiguous` 可观测 |
| 服务器行为与文档漂移 | 中 | 中 | 先观战再落笔；记录不一致并提 issue；解码保持宽松但校验必填字段 |
| 平台限制（仅 POSIX） | 高 | 低 | 明确 v1 支持范围；Windows 需自定义 transport |

---

## 11. 未决问题（需要评审/用户确认）

| 编号 | 问题 | 建议默认 |
| --- | --- | --- |
| Q1 | 重连策略 | 已定为显式 `reconnect()`；退避 helper 只放示例，不进核心库 |
| Q2 | `codec` 是否作为独立公开 target？ | 是；便于测试与工具 |
| Q3 | `Painter` 内置策略的程度？ | 内置基础策略，允许完全绕过，只用 `Client` |
| Q4 | 交付形态 | 静态库 + `install`/export；不做 header-only |
| Q5 | 目录名 | 保持 `pb2027-cpp-sdk/`（根目录已叫 `pb2027`，也可缩成 `sdk/`） |
| Q6 | 服务器 host / 证书 | 需要用户提供；自签则用 `ca_file`，默认仍严格校验 |
| Q7 | 观战端范围 | M5 保留 `Board` + `/api/board`；渲染与播放器不做 |
| Q8 | 平台范围 | v1 只保证 Linux x86_64；macOS 可测；Windows 不做 |
| Q9 | 日志接口 | `std::function<void(LogLevel, string_view)>`，不引入 spdlog |
| Q10 | 是否要 `GET /api/board` | 提供，返回结构化 `BoardResponse` |
| Q11 | `submit()` 编码失败/超额时 | 只入队；`flush()` 编码失败 → `on_error` 并丢弃该批，不自动重试 |
| Q12 | `Board` 尺寸 | Option B：`Snapshot` 是唯一初始化来源，`PUBLIC` 先到则报错 |

---

## 12. 附录

### 12.1 协议索引

| 主题 | 章节 |
| --- | --- |
| 画笔与 token | §1.2 |
| 落笔判定与冷却 | §1.3 |
| Tick | §1.4 |
| 声纳与公共画面 | §1.5 |
| HTTP 通用 | §2.1 |
| 申领 token | §2.2 |
| meta | §2.3 |
| board | §2.4 |
| WS 连接 | §3.1 |
| zstd 帧 | §3.2 |
| 载荷规则 | §3.3 |
| BATCH | §3.4 |
| 槽位 | §3.5 |
| 结果码 | §3.6 |
| 下行消息 | §3.7 |
| TICK 生成 | §3.8 |
| 公共流 | §3.9 |
| 心跳 | §3.10 |
| 限流/关闭码 | §3.11 |
| 客户端建议 | §5 |

### 12.2 参考

- 协议文档：`../protocol.md`
- 外部评审：`../suggestions-gpt-6.1-sol.md`
- TypeScript 参考实现：`../basicpainter.ts`
- websocketclient-cpp：<https://github.com/rbeeli/websocketclient-cpp>（锁 commit `96a3f3f`）
- cpp-httplib：<https://github.com/yhirose/cpp-httplib>（0.66.0）
- nlohmann/json：<https://github.com/nlohmann/json>
- doctest：<https://github.com/doctest/doctest>

### 12.3 术语

| 术语 | 含义 |
| --- | --- |
| 画笔（brush） | 属于 uid 的 `(p, t)`，上次成功落笔位置与时间 |
| 槽位（slot） | 连接内的 token 句柄，0…max_slots-1 |
| 绑定（bind） | 把 token 放到槽位；解绑用 16 个 0 |
| 声纳（sonar） | 落笔后返回的菱形像素快照，`A = 2R² + 2R + 1`（默认 13） |
| `unsent` | 还在 outbound 队列、从未写出 socket 的记录 |
| `ambiguous` | 已写出 socket、但未收到结果的记录（可能已生效） |
| `SubmissionId` | 一次 `submit()`/`flush()` 的本地编号，用于关联结果 |
| 公共版本（Vpub） | 公共画面已应用到的 tick 号 |
| 历史层（l1/l2） | 公共画面最近两个旧颜色，用于 `PUBLIC` 的 0/1 颜色码 |

---

## 13. 评审回应（v0.1 → v0.2）

### 13.1 逐条处理

| 评审点 | 建议 | 处理 | 落点 |
| --- | --- | --- | --- |
| 1. `submit()` 无法关联结果 | `Batch` 携带 wire metadata；`submit` 返回 `SubmissionId` | **采纳** | §5.2、§5.4、§5.5 |
| 2. FIFO 应在写出后建立 | `outbound` / `inflight` 两级队列 | **采纳** | §5.5、§5.5.1、§6.2 |
| 3. 多槽位状态顺序 | `desired` / `active` 双表；结果→lost→brush | **采纳** | §5.5、§6.3 |
| 4. `poll()` 阻塞重连 | 改为显式 `reconnect()`；`poll` 总 deadline + 排空预算 | **采纳** | §5.5、§5.9、§7.2 |
| 5. Pong 行为需实测 | 加 mock 测试；库明确不自动回，保留手动 | **部分采纳** | §6.6、§9.3 |
| 二.1 `order=1` 缺尺寸 | 直接报错，不再“跳过校验” | **采纳（用其备选：保留 wire key，由 `Board` 换算）** | §6.4、§5.4、§5.7 |
| 二.2 差分解码溢出 | checked arithmetic + 新上限字段 | **采纳** | §5.4、§6.4 |
| 二.3 `Board` 尺寸语义 | Option B：Snapshot 决定尺寸 | **采纳** | §5.7 |
| 二.4 Snapshot 历史层 | 显式 `SnapshotCodec` 枚举 | **采纳** | §5.2 |
| 三.1 `304` 建模 | `BoardResponse::Kind` | **采纳** | §5.8 |
| 三.2 多段 PUBLIC | `fetch_board` 在 API 层切分 | **采纳** | §5.8、§6.4 |
| 三.3 Endpoint 缺字段 | `base_path`、代理认证、超时 | **采纳** | §5.8 |
| 四. `basicpainter.ts` | 作为 golden/声纳/冷却参考；补多槽位 | **采纳** | §5.6、§9.1 |
| 五. 工期偏乐观 | 拆成 M0–M6，12–17 人日 | **采纳** | §8 |
| 六. 新增测试 | 覆盖断线、多 BATCH、槽位、公共流 | **采纳** | §9.3 |
| — | TICK 后剩余数量不一致也算协议错误 | **不采纳**：`nr < inflight` 是正常的，只有 `nr > inflight` 才错 | §6.3 |
| — | 自动重连默认开/关 | **已决定**：核心库不做自动重连，示例提供退避循环 | §0.1 D5、§7.3 |

### 13.2 `basicpainter.ts` 的复用点

- `encodeBatch` 的列偏移与 `pad4` 可以直接作为 C++ golden 向量（比较解压后的 payload，不比较压缩帧）。
- `onTick` 的 `brush.t/idx/slot`、`result.status/value`、`sonar.idx/color` 偏移与本文一致，可作为交叉校验。
- `diamond()` 的 `dy` 外层、`dx` 内层与越界补 0 的顺序，直接转成声纳单测。
- `waitMs()` 的 `ready = brush.t + c0 + k·d` 与“每次等待后重新读 brush”是 v1 调度器的正确参考。
- **不可照搬**：单槽位、单请求同步等待、无 `lost.slot`、无重连、无结果 FIFO；C++ 版必须用多槽位状态机。

### 13.3 v0.1 → v0.2 变更摘要

1. `submit()` 返回 `SubmissionId`；`Result` 增加 `submission`/`record`，去掉裸 `seq`。
2. 引入 `outbound_queue_` / `inflight_`；`unsent` / `ambiguous` 替代 `pending_dropped`。
3. 删除 `ReconnectPolicy` 与 `poll()` 内建连；新增 `reconnect()`，`State` 去掉 `Reconnecting`。
4. `poll()` 改为总 deadline + `max_messages_per_poll`，明确只收不发。
5. `Board` 改为 Option B（`Board()` + `apply(Snapshot)`），`apply(PublicBatch)` 可返回错误。
6. `Snapshot` 增加 `SnapshotCodec` 枚举；`PublicBatch` 解码要求 W/H 已知。
7. `fetch_board` 返回结构化 `BoardResponse`（304 / Snapshot / 多段 Public）。
8. `Endpoint` 补 `base_path`、代理认证、请求超时。
9. 里程碑改为 M0–M6，12–17 人日；真实联调移到 M6。
10. 补 `unsent/ambiguous`、`nr > inflight`、`order=1`、`dsmall=255`、声纳边界等测试。
