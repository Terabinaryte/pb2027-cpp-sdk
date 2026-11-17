# 实现进度

> 更新时间：2026-11-17
> 对应计划：[PLAN.md](PLAN.md) v0.2
> 当前阶段：**M0–M6 完成**（sanitizer 运行时缺失，作为可选补强）

## 环境

| 项 | 值 |
| --- | --- |
| OS | Fedora Linux 45 |
| 编译器 | GCC 16.2.1 |
| 构建 | CMake 4.3.0 + Ninja 1.13.2 |
| zstd | 1.5.7（`zstd::libzstd`） |
| OpenSSL | 4.0.3 |
| zlib | 1.3.1 |
| 服务器 | `https://pb.ika.zip/`（已用 `GET /api/meta` 验证可达） |

## 已完成

### M0 工程骨架

- [x] CMake 工程：`pb2027_codec` 静态库、`pb2027_tests`、`pb2027_ws_smoke`。
- [x] 依赖 vendor（见 [third_party/README.md](third_party/README.md)）：
      websocketclient-cpp `96a3f3f`、doctest 2.4.11、cpp-httplib 0.66.0、nlohmann/json 3.11.3。
- [x] 离线 configure/build：不访问网络。
- [x] 本地 TLS WebSocket echo server（`tests/mock_server.py`，Python 标准库 + 自签证书）。
- [x] TLS WebSocket 冒烟：`pb2027_ws_smoke` 连接 `wss://localhost:9443`，二进制 echo 通过。
- [x] 真实服务器 `/api/meta` 快照保存为 [tests/real_meta.json](tests/real_meta.json)。

冒烟结果：

```
OK echo 12 bytes
```

### M1 codec

- [x] `types.hpp` / `error.hpp` / `codec.hpp`：强类型、`std::expected`。
- [x] `Batch`：bind/unbind/paint，wire 顺序（绑定/解绑在前，绘画在后）。
- [x] `encode_batch`：列布局、`pad4`、小端、alpha 校验、`max_slots`/`max_paints`/尺寸校验、单 zstd frame。
- [x] `decode_frame`：单帧、无字典、窗口 ≤ 8 MiB、解压上限、帧边界。
- [x] `decode_payload`：`RULES`、`TICK`、`SNAPSHOT`（codec 0/1）、`PUBLIC`（含 `dsmall=255`/`dbig`、多段、key 前缀和）、未知类型。
- [x] 严格校验：保留字段、长度、升序、alpha、`Σ seg.n == n`、`dbig` 数量、`from < to`、`seg.tick` 递增。
- [x] 单测：15 个 test case / 115 个断言全部通过。

```
[doctest] test cases:  15 | 15 passed | 0 failed | 0 skipped
[doctest] assertions: 116 | 116 passed | 0 failed |
```

### M2 会话层

- [x] `src/ws_transport.cpp`：websocketclient-cpp 适配（TLS、握手、读、写、Pong、Close）。
- [x] `Client`：connect / reconnect / poll / flush / close，`outbound`/`inflight`、`desired`/`active`、结果关联、Ping/Pong、关闭码。
- [x] 脚本化协议服务器 `tests/protocol_server.py`（RULES/TICK/COOLING/Close）。
- [x] 客户端测试：connect、bind+paint、COOLING、`submit`、显式重连、服务端 4000 关闭；43 个断言通过。
- [x] **真机只读联调**：`wss://pb.ika.zip/ws?public=1` 连接成功，解码 RULES、7.2 MB SNAPSHOT（codec 1）、连续 PUBLIC 流。

真机输出：

```
RULES 1000x600 tps=128 slots=8000 c0=1000000 k=100000
CONNECTED
SNAPSHOT 1000x600 v=17734448 codec=1 pixels=600000 l1=600000
PUBLIC (17734448,17734576] segs=32 order=0
PUBLIC (17734576,17734704] segs=32 order=0
...
```

### M3 失败路径

- [x] `lost.slot` 清空 active/desired；服务端 4000 关闭 → 非 retryable；TCP 断开 → `ambiguous` 计数。
- [x] 显式 `reconnect()`：重连 + 重绑 + `since`；重连后继续落笔。
- [x] 3 个客户端 test case / 69 断言。

### M5 Board（子代理 board-dev）

- [x] `Board` / `SonarTable`：Snapshot codec 0/1、PUBLIC order 0/1 的 key→idx、历史层 0/1/2、声纳 overlay/prune。
- [x] 22 个 test case / 186 断言通过；`-Wall -Wextra -Wpedantic -Wshadow -Wconversion` 下 0 警告。
- [ ] HTTP `api`（api-dev 实现中）。

### M5 HTTP api（子代理 api-dev）

> 多 token 画笔调度器已从 SDK 移出到独立仓库 `pb2027-cpp-client`（稍后开源）；SDK 只保留 codec/client/board/api。

- [x] `include/pb2027/api.hpp` / `src/http.cpp` / `src/api.cpp`。
- [x] `fetch_meta`、`acquire_token`（access_key / captcha+paste）、`fetch_board`（304 / Snapshot / 多段 PUBLIC）。
- [x] `/api/board` 解码上限放宽到 64 MiB，新增 7,200,016 字节 codec-1 Snapshot 回归测试。
- [x] 10 个 test case / 132 断言通过；实机只发 1 次 `GET /api/meta`。

接口偏差（已确认）：

- `fetch_meta` 额外填充 `rules.width/height` 与 `rules.server_time_us`。
- 非 200 除 429 外统一返回 `Protocol`，未细分 403 `Banned`。
- `Endpoint.host` 容忍 `https://` / `http://` 前缀。

### M6 安装与真机联调

- [x] `cmake --install` + 独立 consumer 工程 `find_package(pb2027)` 构建/运行通过。
- [x] `examples/viewer.cpp`：真机 `public=1`，应用 7.2 MB SNAPSHOT 与连续 PUBLIC，读到像素颜色。
- [x] `examples/paint_probe.cpp`：测试模式（`issuance.access_mode == "none"`）下批量申请 token、绑定、同色重涂、COOLING、重连重绑。
- [x] 真机落笔：5 / 30 / 100 / 300 个 token 全部 bind + paint OK，各自产生 sonar；立即重涂返回 COOLING；`reconnect()` 后 300 个槽位自动重绑并继续落笔 OK。
- [x] `api::UidRequest`：测试模式用 `{"uid": N}` 申领 token（`access_mode=none` 时服务器要求）。
- [x] **50×50 白色矩形**：`(0,0)` 共 2500 像素，300 个 token（每 token 负责一行内 8–9 个连续像素），9 波、32.4 s 全部 OK；公共画面延迟后校验 50×50 全白（`non-white=0`）。示例：`examples/paint_rect.cpp`。

真机落笔输出（300 tokens，并行申请 token，约 24 s）：

```
meta: board 1000x600 max_slots=8000
tokens: 300 (uid 940001..940300)
connected
bound 300 slots
painted 300/300 OK, sonars=300
cooling after 705086 us
reconnected and rebound 300 slots
post-reconnect paint OK
done
```

> 落笔测试使用画布底行、重涂像素当前颜色，不改变画面；测试 uid 段为 910001–910100、920001–920100。
> 备注：`paint_probe` 以 16 路并发申请 token；300 个 token 的完整流程约 24 s。

### 当前测试矩阵

| 测试 | 用例 | 断言 | 备注 |
| --- | --- | --- | --- |
| `pb2027_codec_tests` | 15 | 116 | 纯离线 |
| `pb2027_board_tests` | 22 | 186 | 纯离线 |
| `pb2027_client_tests` | 2 | 55 | 本地 TLS mock server |
| `pb2027_api_tests` | 10 | 133 | 1 次真实 /api/meta；默认不进 ctest |
| 合计 | 49 | 490 | |

### 真机与文档的差异（重要）

1. **RULES offset 2 不是保留 0**：真机写 `0x0020`（32），很可能就是 `public.tps`（文档 §1.5 的默认值 32，但没写进 RULES）。解码器已把它暴露为 `Rules.public_tps`，不再拒绝。
2. **下行 SNAPSHOT 远大于 `max_payload_bytes`**：1000×600 codec 1 快照解压后 7,200,016 字节，压缩后 1,320,992 字节，而 `RULES.max_payload_bytes = 1 MiB`。这说明 §3.11 的 1 MiB 只适用于客户端上行；SDK 解码下行时按 `16 + 12·W·H` 放宽了上限，WS 读缓冲也放到 16 MiB。
3. `max_slots=8000`、`c0=1 s`、`k=0.1 s/px`、`public_delay=60 s` 与文档默认值不同（已记录在下方）。

## 真实服务器参数（2026-11-17 抓取）

| 参数 | 值 | 与文档默认不同 |
| --- | --- | --- |
| 画布 | 1000 × 600 | — |
| `tps` | 128 | — |
| `cooldown_base_us` | 1,000,000（1 s） | 文档默认 10 s |
| `cooldown_per_px_us` | 100,000（0.1 s/格） | 文档默认 1 s/格 |
| `max_slots` | 8000 | 文档默认 4096 |
| `public_delay_ms` | 60,000（1 min） | 文档默认 10 min |
| `sonar_radius_px` | 2 | — |
| `captcha_provider` / `access_mode` | `none` / `none` | 当前不需要验证码 |
| `public_version` | 约 1765 万（活动进行中） | — |

> 这些值在运行时以 `/api/meta` 或 `RULES` 为准；测试里同时覆盖文档默认值和真实值（`max_slots=8000`）。

## 构建与测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

# M0 传输冒烟（两个终端）
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout tests/tls/key.pem -out tests/tls/cert.pem -days 2 \
  -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
python3 tests/mock_server.py --port 9443 \
  --cert tests/tls/cert.pem --key tests/tls/key.pem
./build/pb2027_ws_smoke localhost 9443
```

## 实现中发现的问题

- `websocketclient-cpp` 的 `send_message` 会**原地用 mask key 改写传入缓冲区**（`Message::data` 是非 owning span）。SDK 必须在发送后丢弃该 buffer，不能复用或事后比较；冒烟测试里最初就踩到这个坑。
- `ZSTD_frameHeader` / `ZSTD_getFrameHeader` 需要 `ZSTD_STATIC_LINKING_ONLY` 才在 `zstd.h` 中声明；已在该翻译单元定义。
- 真实服务器 `max_slots=8000`、冷却参数比文档默认值小很多，调度器的测试不能只按默认值写。

## 后续优化

1. 补 `unsent`/`ambiguous` 的断线计数测试、`max_slots` 热调、结果数异常。
2. `Client` 多 `on_tick` 监听器；`Painter` 已移到独立仓库 `pb2027-cpp-client`。
3. 真实服务器小批量落笔联调与攻击/反击实测（client 仓库负责调度）。
