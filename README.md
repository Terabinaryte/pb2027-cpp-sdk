# pb2027 C++ SDK

pb2027（ikapb 协议）的 C++23 客户端 SDK。当前完成 **M0 工程骨架 + M1 codec**，
计划与设计见 [PLAN.md](PLAN.md)，实现进度见 [PROGRESS.md](PROGRESS.md)。

## 当前能力

- `pb2027::codec`：`BATCH` 编码，`RULES` / `TICK` / `SNAPSHOT` / `PUBLIC` 解码（纯协议层，无网络依赖）。
- `pb2027::client`：TLS WebSocket 会话、绑定/绘画、结果关联、重连、Ping/Pong。
- `pb2027::viewer`：`Board` / `SonarTable` 公共画面与声纳叠加。
- `pb2027::api`：可选的 HTTP 模块（meta / paint-token / board）。
- 严格按 `protocol.md` 校验单 zstd frame、长度、`pad4`、alpha、升序、差分解码等。
- `pb2027_ws_smoke`：TLS WebSocket 握手 + 二进制 echo 冒烟（M0）。

已在测试服完成真实落笔（5/30/100/300 token）、冷却、重连重绑验证；多 token 调度器在独立仓库 `pb2027-cpp-client`，不在 SDK 内。Windows 支持不在 v1 范围。

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

# 真实 /api/meta 测试（默认关闭，会联网）
cmake -S . -B build -DPB2027_ENABLE_NETWORK_TESTS=ON
./build/pb2027_api_tests
```

## 示例

```bash
./build/pb2027_real_probe pb.ika.zip 443      # 只读：RULES + SNAPSHOT + PUBLIC
./build/pb2027_viewer_demo pb.ika.zip 443     # 只读：Board 应用真实公共画面
./build/pb2027_paint_probe pb.ika.zip 300 940001  # 测试服写路径：300 个 token
./build/pb2027_paint_rect pb.ika.zip 0 0 50 50 950001 300  # 测试服：50x50 白块
```

`paint_probe` 只在 `/api/meta` 的 `issuance.access_mode == "none"`（测试环境）下工作，会为 uid 段申请 token；生产服请勿运行。

依赖全部 vendor 在 `third_party/`，构建不需要联网。系统依赖：zstd、OpenSSL、zlib（开发头文件）。

## 目录

```
include/pb2027/   公共头（只依赖标准库）
src/codec.cpp     协议编解码
tests/            单测、mock server、传输冒烟
third_party/      vendor 依赖
```

## 文档

- [SDK 使用文档](docs/SDK.md)：安装、快速开始、API 参考、运行时契约、已知限制
- [设计计划](PLAN.md)
- [实现进度](PROGRESS.md)

## 协议

以仓库根目录 `../protocol.md` 为准（SHA-256 `25ccf58a…`），本 SDK 的设计文档见 [PLAN.md](PLAN.md)。
