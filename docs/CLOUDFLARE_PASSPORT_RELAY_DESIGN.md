# Kiro Passport Cloudflare Relay：设计与实施

## 1. 目标与范围

本设计将 Kiro Passport 从已移除的 BLE 本地桥接模式迁移为 **Wi-Fi + WSS 长连接硬件审批器**。

设备在连接 Wi-Fi 并完成可信时间同步后，主动建立以下长期连接：

```text
wss://ws.yanyunnnx.cc.cd/device/<device_id>
```

Cloudflare 端将审批请求下发给设备，设备通过实体按键作出允许或拒绝选择，Cloudflare Durable Object（DO）将结果返回给 Kiro Hook 使用的本机 HTTP client。

本阶段范围：

- Passport 固件仅通过 Wi-Fi 通信，不使用 BLE。
- Cloudflare Worker 提供公网 relay 和 HTTP API。
- 每台设备由一个 Durable Object 串行管理。
- Worker/DO 必须在请求超时、设备离线、协议校验失败时默认 `deny`。
- Cloudflare 部署/API 凭据不得写入 Passport 固件、NVS 或 Git。

非本阶段范围：

- 语音、音频或摄像头数据传输。
- 设备与 Cloudflare 的 mTLS；初版使用设备专属 Bearer credential + 受信任 TLS CA 验证。
- 允许公网匿名用户直接请求设备审批。

## 2. 设计原则

1. **默认拒绝**：未收到已验证的 `allow`，所有结果均为拒绝。
2. **一台设备、一个串行协调器**：使用一个以 `device_id` 命名的 Durable Object 防止并发审批竞态。
3. **不信任网络输入**：设备和 Worker 均验证版本、身份、会话、请求 ID、到期时间及消息结构。
4. **设备凭据最小化**：设备仅持有本设备的可轮换 credential，绝不保存 Cloudflare API Token、Wrangler token、D1 管理凭据或管理端 API Key。
5. **持久状态优先**：DO storage 保存 pending request 和到期信息；不依赖内存 Promise 等待审批。
6. **连接不等于授权**：即使 WebSocket 已完成 TLS，审批结论仍需匹配 `device_id`、`session_id`、`request_id` 和 `expires_at`。
7. **私密凭据需加密静态存储**：固件只有在启用 NVS encryption 后才能持久化设备 credential。

## 3. 总体架构

```text
┌─────────────────────┐
│ Kiro Hook            │
│ / Python HTTP client │
└──────────┬──────────┘
           │ HTTPS: POST /v1/devices/:id/requests
           │        GET  /v1/requests/:request_id
           ▼
┌───────────────────────────────────────────┐
│ Cloudflare Worker                          │
│ - Hook/API 身份验证                         │
│ - 请求格式、限流和审计入口                 │
│ - 按 device_id 路由到 Durable Object       │
└───────────────────┬───────────────────────┘
                    │ idFromName(device_id)
                    ▼
┌───────────────────────────────────────────┐
│ PassportRelay Durable Object               │
│ - 当前设备 WebSocket                        │
│ - 当前 session_id                           │
│ - pending requests / decisions              │
│ - alarm 超时拒绝                            │
│ - 单设备串行状态机                          │
└───────────────────┬───────────────────────┘
                    │ WSS
                    ▼
┌───────────────────────────────────────────┐
│ Passport (ESP32-C3)                        │
│ wss://ws.yanyunnnx.cc.cd/device/<id>       │
│ Authorization: Bearer <device credential>  │
│ Wi-Fi + NTP + TLS + 实体按键审批            │
└───────────────────────────────────────────┘

          ┌─────────────────────────────┐
          │ Cloudflare D1               │
          │ devices / approval_audit    │
          └─────────────────────────────┘
```

## 4. 域名与路由

本项目的设备 relay 域名固定为：

```text
ws.yanyunnnx.cc.cd
```

设备固件注册的 relay URL 应为：

```text
wss://ws.yanyunnnx.cc.cd
```

固件自动追加设备路径，因此最终连接地址是：

```text
wss://ws.yanyunnnx.cc.cd/device/passport-AABBCCDDEEFF
```

Worker 路由约定：

| 路径 | 方法 | 调用方 | 用途 |
|---|---:|---|---|
| `/device/:device_id` | `GET` + WebSocket Upgrade | Passport | 已认证设备长期 WSS 连接 |
| `/v1/admin/devices` | `POST` | 管理端/工厂工具 | 注册设备、一次性签发 credential |
| `/v1/admin/devices/:device_id/rotate` | `POST` | 管理端/工厂工具 | 轮换设备 credential |
| `/v1/devices/:device_id/requests` | `POST` | Kiro Hook HTTP client | 创建审批请求 |
| `/v1/requests/:request_id` | `GET` | Kiro Hook HTTP client | 查询审批终态 |
| `/healthz` | `GET` | 运维 | 仅检查 Worker 可达性，不泄露设备状态 |

应在 Cloudflare Dashboard 或 Wrangler 中将 `ws.yanyunnnx.cc.cd` 绑定为 Worker Custom Domain；DNS 必须由 Cloudflare 代理，才能由 Worker 接收 WebSocket Upgrade。

## 5. 身份、凭据与数据存储

### 5.1 三类凭据

| 凭据 | 保管位置 | 允许用途 | 禁止用途 |
|---|---|---|---|
| Cloudflare API / Wrangler token | 开发者本机、CI secret | 部署 Worker、管理 Cloudflare | 固件、Git、D1、设备 NVS |
| 管理端 API key | Workers Secret | 注册/停用/轮换设备 | 设备连接、Hook 日常请求 |
| 设备 credential | 加密 NVS + D1 hash | Passport WSS 鉴权 | 部署、管理其他设备 |
| Hook user token | 本机环境变量或 Cloudflare Access service token | 创建/查询审批请求 | 设备 WSS 鉴权 |

### 5.2 设备 credential

设备 credential 必须由 Worker 使用加密安全随机源签发，至少 32 byte（256 bit）随机熵；向工厂工具仅返回一次。D1 只保存其 hash，而不保存明文。

建议表结构：

```sql
CREATE TABLE devices (
  device_id TEXT PRIMARY KEY,
  credential_hash TEXT NOT NULL,
  credential_version INTEGER NOT NULL DEFAULT 1,
  status TEXT NOT NULL DEFAULT 'active',
  created_at INTEGER NOT NULL,
  rotated_at INTEGER
);

CREATE TABLE approval_audit (
  request_id TEXT PRIMARY KEY,
  device_id TEXT NOT NULL,
  session_id TEXT NOT NULL,
  tool TEXT NOT NULL,
  summary TEXT NOT NULL,
  decision TEXT NOT NULL,
  reason TEXT NOT NULL,
  expires_at INTEGER NOT NULL,
  decided_at INTEGER NOT NULL
);
```

`credential_hash` 推荐保存 `SHA-256(credential || server_pepper)`；`server_pepper` 置于 Workers Secret。验证时必须使用恒定时间比较。

### 5.3 固件写入 credential

当前固件的 `kiro_passport_network_configure(relay_url, credential)` 明确拒绝在未启用 NVS encryption 时保存 credential。

生产写入流程应为：

1. 生产配置启用 NVS encryption，并完成 ESP-IDF NVS key / flash encryption 的受控烧录方案。
2. 工厂系统调用设备注册 API，获得一次性返回的 device credential。
3. 经受控串口、工厂治具或安全 provisioning 工具调用设备配置接口。
4. 写入 `wss://ws.yanyunnnx.cc.cd` 与 device credential。
5. 工厂工具清理内存、日志和临时文件中的 credential。

禁止通过 Wi-Fi 配网页提交设备 credential。该页面只用于家庭/现场的 Wi-Fi 路由器配置。

## 6. Durable Object 状态机

每个 `device_id` 映射为一个 `PassportRelay` Durable Object：

```ts
const id = env.PASSPORTS.idFromName(deviceId);
const stub = env.PASSPORTS.get(id);
```

DO 应持久化：

```ts
type PendingRequest = {
  requestId: string;
  deviceId: string;
  sessionId: string;
  tool: string;
  summary: string;
  expiresAt: number;
  status: "pending" | "allow" | "deny" | "timeout" | "offline";
  decision?: "allow" | "deny";
  reason?: string;
};
```

状态迁移：

```text
created
  ├─ device offline ────────────> deny / offline
  ├─ request expires ───────────> deny / timeout
  ├─ valid device allow ────────> allow / user
  ├─ valid device deny ─────────> deny / user
  ├─ session disconnects ───────> deny / session_lost
  └─ malformed/replay message ──> deny / protocol_error
```

约束：

- 每设备只允许一个 `pending` 高风险审批。
- 同一 `request_id` 只能进入一个终态。
- 保存请求后立即设置最早的 alarm。
- `alarm()` 必须将所有已到期的 pending request 原子写为 `deny/timeout`。
- 同一设备新建 WebSocket 连接时应关闭旧 socket，防止双连接竞争。
- DO 应使用 WebSocket hibernation API；连接空闲时不应依赖常驻内存状态。

## 7. WebSocket 协议 v1

所有 WebSocket payload 为 UTF-8 JSON object，最大消息长度应限制为 512 byte（固件现有限制）。未知字段、重复字段、转义字符串、控制字符、超长字段或版本不符均应拒绝。

### 7.1 设备上线

设备认证通过并完成 WSS Upgrade 后发送：

```json
{
  "v": 1,
  "type": "hello",
  "device_id": "passport-AABBCCDDEEFF",
  "session_id": "32-byte-random-hex"
}
```

DO 校验：

- socket 已通过该设备 credential 鉴权；
- `device_id` 必须等于 URL 与 DO 所属 device；
- `session_id` 必须非空、格式合法；
- 将 session 绑定为当前设备会话。

### 7.2 Worker 下发审批请求

```json
{
  "v": 1,
  "type": "request",
  "device_id": "passport-AABBCCDDEEFF",
  "session_id": "current-session-id",
  "request_id": "server-generated-128-bit-id",
  "tool": "shell.execute",
  "summary": "Run deployment command",
  "expires_at": 1780000000
}
```

要求：

- `request_id` 必须由 Cloudflare 端生成，不接受 Hook 自行指定。
- `expires_at` 使用 Unix epoch 秒数。
- 建议 Hook 请求最大 TTL 设为 60–120 秒；固件目前拒绝超过 300 秒的请求。
- 发往设备前，DO 必须先持久化 pending request 和 alarm。

### 7.3 设备返回决策

```json
{
  "v": 1,
  "type": "decision",
  "device_id": "passport-AABBCCDDEEFF",
  "session_id": "original-session-id",
  "request_id": "original-request-id",
  "decision": "allow",
  "reason": "user"
}
```

DO 必须同时验证：

1. WebSocket 已认证为该 `device_id`。
2. `device_id` 与 DO 相同。
3. `session_id` 与 pending request 创建时保存的 session 相同。
4. `request_id` 存在、未完成且未到期。
5. `decision` 仅能是 `allow` 或 `deny`。

任一条件不满足时，DO 不得产生 `allow`。应记录审计事件，并保持或转为 deny。

### 7.4 决策确认（下一固件迭代必做）

DO 成功持久化终态后返回：

```json
{
  "v": 1,
  "type": "decision_ack",
  "request_id": "original-request-id",
  "session_id": "original-session-id",
  "accepted": true
}
```

当前固件在 WebSocket 写入成功后会清除本地 decision；这不代表 DO 已接受该决策。下一固件迭代必须改为：仅在匹配的 `decision_ack` 到达后清除本地 decision；否则重试或由 DO 的 timeout 兜底拒绝。

## 8. Hook HTTP API

### 8.1 创建请求

```http
POST /v1/devices/passport-AABBCCDDEEFF/requests
Authorization: Bearer <hook-user-token>
Content-Type: application/json

{
  "tool": "shell.execute",
  "summary": "Run deployment command",
  "ttl_seconds": 60
}
```

成功时返回：

```http
HTTP/1.1 202 Accepted

{
  "request_id": "server-generated-id",
  "status": "pending",
  "expires_at": 1780000000
}
```

设备离线或不存在 pending socket 时，建议直接返回 deny 状态，而不是创建无法交付的 allow 请求：

```json
{
  "request_id": "server-generated-id",
  "status": "deny",
  "reason": "offline"
}
```

### 8.2 查询结果

```http
GET /v1/requests/<request_id>
Authorization: Bearer <hook-user-token>
```

返回仅可为：

```json
{ "status": "pending" }
{ "status": "allow", "reason": "user" }
{ "status": "deny", "reason": "user" }
{ "status": "deny", "reason": "timeout" }
{ "status": "deny", "reason": "offline" }
{ "status": "deny", "reason": "session_lost" }
```

本机 Python client 应采用 `POST + polling`：每 0.5–1 秒轮询一次至终态或超时。不要在单个 Worker HTTP 请求中长时间等待实体按键审批，因为持久化状态和 DO alarm 才是可靠的超时机制。

Hook 只能在 `status == "allow"` 时执行受保护操作；其他任意状态全部按 deny 处理。

## 9. Cloudflare Worker 实施结构

建议新增独立目录：

```text
cloudflare/
├── package.json
├── wrangler.toml
├── migrations/
│   └── 0001_devices.sql
└── src/
    ├── index.ts          # HTTP router / auth / DO forwarding
    ├── passport-relay.ts # PassportRelay Durable Object
    ├── auth.ts           # 管理端、Hook、设备 credential 校验
    ├── protocol.ts       # strict schema/type guards
    └── db.ts             # D1 operations and audit helpers
```

`wrangler.toml` 骨架：

```toml
name = "kiro-passport-relay"
main = "src/index.ts"
compatibility_date = "2026-08-23"

[[durable_objects.bindings]]
name = "PASSPORTS"
class_name = "PassportRelay"

[[migrations]]
tag = "v1"
new_sqlite_classes = ["PassportRelay"]

[[d1_databases]]
binding = "DB"
database_name = "kiro-passport"
database_id = "<D1_DATABASE_ID>"
```

使用 Worker Secret 保存服务端秘密：

```sh
wrangler secret put ADMIN_API_KEY
wrangler secret put HOOK_AUTH_SECRET
wrangler secret put DEVICE_CREDENTIAL_PEPPER
```

不得把这些值放进：

- `wrangler.toml`；
- Git；
- Passport 固件；
- 设备 NVS；
- Kiro Hook 源代码字面量。

## 10. 实施顺序

### 阶段 A：Cloudflare 基础设施

1. 在 Cloudflare 创建 Worker、D1 数据库和 Durable Object binding。
2. 将 `ws.yanyunnnx.cc.cd` 绑定为 Worker Custom Domain。
3. 执行 D1 migration，建立 `devices` 和 `approval_audit` 表。
4. 使用 `wrangler secret put` 注入管理端、Hook 和 pepper secret。
5. 部署 `/healthz`，验证域名、TLS 和 Worker 可达性。

### 阶段 B：设备注册与 WSS

1. 实现管理端设备注册 API。
2. 签发 256-bit device credential，D1 仅保存 hash。
3. 实现 `/device/:device_id` 的 Bearer 验证与 WebSocket Upgrade。
4. 将连接转交给对应 Durable Object。
5. 实现 `hello`、单连接替换和 session 绑定。
6. 在真实设备上启用加密 NVS，并写入 `wss://ws.yanyunnnx.cc.cd` 与 credential。
7. 验证设备页面在 Wi-Fi/NTP 后显示 `Relay ready`。

### 阶段 C：审批与 Hook

1. 实现 DO 的 request 持久化、向设备下发、alarm 超时 deny。
2. 实现 device `decision` 的完整四元组校验。
3. 实现 `POST /requests` 和 `GET /requests/:id`。
4. 将本机 Python bridge 改为 HTTP client：创建请求、轮询终态、只在 allow 时放行 Kiro 操作。
5. 实现 `decision_ack` 并更新固件，使其在收到 ack 前保留本地 decision。

### 阶段 D：轮换、审计和防御

1. 实现 credential rotation：短暂允许新旧凭据并存，设备更新后撤销旧凭据。
2. 对管理 API、Hook API、WSS 认证失败实施按 device/IP 的速率限制。
3. 记录认证失败、重放、过期、session mismatch、超时和 deny 审计事件。
4. 为 `device_id`、`request_id`、`session_id` 增加结构和长度限制。
5. 设置设备停用、撤销和丢失设备处置流程。

## 11. 安全验收清单

- [ ] Passport 不含 Cloudflare API/Wrangler/管理 token。
- [ ] D1 不保存 device credential 明文。
- [ ] 设备 credential 只保存到加密 NVS。
- [ ] 固件仅连接 `wss://ws.yanyunnnx.cc.cd/device/<device_id>`。
- [ ] TLS 证书、主机名和系统时间校验均成功后才连接。
- [ ] Worker 验证 URL device ID、Bearer credential 和设备状态。
- [ ] DO 对每个决策校验 `device_id + session_id + request_id + expires_at`。
- [ ] 请求到期、设备离线、断线、消息异常或重复 request 时均为 deny。
- [ ] Durable Object `alarm()` 可在无活动连接时处理 timeout。
- [ ] Hook 仅在明确 `allow` 后执行高风险操作。
- [ ] 设备 credential 轮换和设备撤销经真实设备测试。
- [ ] 设备断电、Wi-Fi 断开、Worker 重启和 DO hibernation 后均不会错误 allow。

## 12. 验证场景

| 场景 | 期望结果 |
|---|---|
| 正常 allow | Passport 点击 OK，DO 记录 allow，Hook 执行操作 |
| 正常 deny | Passport 点击 DOWN，DO 返回 deny，Hook 不执行 |
| 设备离线 | 创建请求直接 deny/offline 或至多在 TTL 后 deny |
| 设备不响应 | alarm 到期后 deny/timeout |
| Wi-Fi/WSS 中断 | 未决请求 deny/session_lost；不得在重连后继续 allow |
| 错误 device_id | Worker/DO 拒绝且不泄露设备状态 |
| 错误 session_id | DO 拒绝；不改变 pending request |
| 重放 request_id | DO 拒绝并记录安全事件 |
| 修改 expires_at | DO 按自身持久化的 expires_at 判定，不信任客户端更新 |
| 旧 credential | 验证失败并关闭 WSS |
| 新 credential | 连接成功，旧 credential 在轮换窗口结束后失效 |

## 13. 参考资料

- [Cloudflare Durable Objects WebSocket best practices](https://developers.cloudflare.com/durable-objects/best-practices/websockets/)
- [Cloudflare Durable Object alarms](https://developers.cloudflare.com/workers/runtime-apis/handlers/alarm/index.md)
- [Cloudflare WebSocket hibernation example](https://developers.cloudflare.com/durable-objects/examples/websocket-hibernation-server/)
- [Cloudflare WebSockets runtime documentation](https://developers.cloudflare.com/workers/runtime-apis/websockets/)

本文中基于 Cloudflare 文档的内容均已为遵守许可限制而重新表述。
