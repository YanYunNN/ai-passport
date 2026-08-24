# Kiro Passport Relay 部署与联调

本目录是独立的 Cloudflare Worker 工程。它实现：每台 Passport 一个 Durable Object、WSS 长连接、D1 中仅保存设备 credential 的带 pepper SHA-256 哈希、管理注册/轮换 API，以及 Hook 的 `POST + polling` 审批 API。

> **安全边界**：Cloudflare API token、`ADMIN_API_KEY`、`HOOK_AUTH_SECRET`、`DEVICE_CREDENTIAL_PEPPER`、`PAIR_CONFIRMATION_SECRET` 和设备 credential 都是秘密。绝不可提交到 Git、写入 `wrangler.toml`，也不可把 Cloudflare 管理凭据写入设备。设备只保存它自己的 WSS credential，且只能写入启用了 NVS encryption 的分区。

## 0. 部署前条件

1. Cloudflare 账户已启用 Workers、Durable Objects 与 D1，且 `yanyunnnx.cc.cd`（或它的实际父 zone）已托管到 Cloudflare。
2. `ws.yanyunnnx.cc.cd` 没有与其他 Worker/Pages 冲突的路由。Worker Custom Domain 的 DNS 由 Cloudflare 代理；不要创建 DNS-only（灰云）记录。
3. 本机已安装 Node.js 22+、npm 和 Cloudflare 账户可用的 API Token。该 token 仅在开发机或 CI secret 中保存，至少授予此账户/zone 所需的 Workers Scripts、Workers Routes、D1、Durable Objects 编辑权限。
4. 从本目录执行命令：

   ```sh
   cd cloudflare
   npm install
   npx wrangler login
   ```

   `package.json` 已固定 Wrangler `4.125.0`。`npm install` 会生成 `package-lock.json`，应将 lockfile 一并提交；`node_modules/` 不提交。

## 1. 创建 D1 并配置 Worker

1. 创建数据库并记录输出中的 `database_id`：

   ```sh
   npx wrangler d1 create kiro-passport
   ```

2. 将 `wrangler.toml` 中的 `REPLACE_WITH_D1_DATABASE_ID` 替换为这个 ID。不要修改 `database_name`、Durable Object class 名或已部署的 migration tag。
3. 在首次部署前执行数据库 migration：

   ```sh
   npm run db:migrate:remote
   ```

   验证表已存在（输出不应包含 SQL error）：

   ```sh
   npx wrangler d1 execute kiro-passport --remote \
     --command "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name"
   ```

   应至少显示 `devices`、`approval_requests`、`approval_audit`。

## 2. 写入 Worker Secrets

所有命令都采用交互输入，避免秘密出现在 shell history。为每个环境生成不同的长随机值，例如 `openssl rand -base64 48`，然后在提示时粘贴。

```sh
npx wrangler secret put ADMIN_API_KEY
npx wrangler secret put HOOK_AUTH_SECRET
npx wrangler secret put DEVICE_CREDENTIAL_PEPPER
npx wrangler secret put PAIR_CONFIRMATION_SECRET
```

用途：

- `ADMIN_API_KEY`：仅工厂/管理端调用设备注册和轮换 API。
- `HOOK_AUTH_SECRET`：仅本机 Kiro Hook 或受信任的桥接进程调用审批 API。
- `DEVICE_CREDENTIAL_PEPPER`：只用于 Worker 中对设备 credential 进行哈希；丢失或更改它会令现有设备无法认证。
- `PAIR_CONFIRMATION_SECRET`：用于签名后台配对页的一次确认令牌；用高熵独立随机值配置，绝不复用 credential pepper 或其他密钥。

生产建议通过 CI 的 secret store 注入 Worker secrets。不要用 `wrangler.toml` 的 `[vars]`、`.env`、设备 NVS 或 Kiro Hook 源代码保存它们。

## 3. 部署、域名和健康检查

部署：

```sh
npm run typecheck
npm run deploy
```

第一次部署会创建 `PassportRelay` Durable Object class。随后仅修改 DO 代码不需要新的 migration；如果未来重命名 class，必须先根据 Cloudflare 的 DO migration 文档显式迁移。

`wrangler.toml` 已声明 Worker Custom Domain：`ws.yanyunnnx.cc.cd`。部署后在 Cloudflare Dashboard **Workers & Pages → kiro-passport-relay → Settings → Domains & Routes** 确认域名状态为 Active；若该域名的父 zone 未在当前账号，先将它移入/委派到 Cloudflare。

验证公网 TLS 与 Worker：

```sh
curl --fail --silent --show-error https://ws.yanyunnnx.cc.cd/healthz
# 预期：{"ok":true}
```

若失败，先检查：D1 ID 是否替换、Custom Domain 是否 Active、DNS 是否被 Cloudflare 代理、API Token 是否有 route/domain 权限。`/healthz` 只表明 Worker 可达，绝不表示某设备在线。

## 4. 注册一台设备（credential 仅显示一次）

`device_id` 是固件按 Wi-Fi STA MAC 生成的 `passport-` 加 12 位大写十六进制，例如 `passport-AABBCCDDEEFF`。先通过工厂日志/治具确认真实值，再注册：

```sh
export RELAY=https://ws.yanyunnnx.cc.cd
read -rs ADMIN_KEY; echo
curl --fail --silent --show-error -X POST "$RELAY/v1/admin/devices" \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -H "Content-Type: application/json" \
  --data '{"device_id":"passport-AABBCCDDEEFF"}'
```

成功响应为 `201`，含一次性的 `credential`。立即将输出交给受控 provisioning 工具，不要保存到普通终端日志、截图、issue 或 Git。D1 只保存 credential hash。

轮换 credential（默认旧 credential 保留 600 秒；可传 60–3600 秒）：

```sh
curl --fail --silent --show-error -X POST \
  "$RELAY/v1/admin/devices/passport-AABBCCDDEEFF/rotate" \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -H "Content-Type: application/json" \
  --data '{"grace_seconds":600}'
```

在 grace window 内把新 credential 写入设备并确认重新上线；窗口过后，旧 credential 的**新连接和存活会话的后续请求/decision**均会被拒绝。Worker 在每次创建审批和处理设备 decision 时都会重新检查 D1 中的设备状态与该 socket 的 credential hash，不能仅依赖握手时的一次认证。

设备遗失或需要立刻收回权限时，调用撤销 API。它先持久化 `revoked` 状态，再通知对应 DO 关闭 socket 并将未决请求置为 `deny/session_lost`：

```sh
curl --fail --silent --show-error -X DELETE \
  "$RELAY/v1/admin/devices/passport-AABBCCDDEEFF" \
  -H "Authorization: Bearer $ADMIN_KEY"
```

撤销是单向状态；如要重新启用设备，使用新的受控管理流程注册/替换 credential，不要仅依赖改 Wi-Fi 密码。

## 5. 设备烧录与 provisioning

当前固件只接受 origin 形式的 relay URL：

```text
wss://ws.yanyunnnx.cc.cd
```

固件自动拼接 `/device/<device_id>`，因此**不要**写入 `/device/...` 路径或尾随 `/`。它还要求 credential 是可打印 ASCII；本 Worker 签发的 base64url credential 满足该要求。

当前仓库的 `sdkconfig` 仍为 `# CONFIG_NVS_ENCRYPTION is not set`。在这种构建下，`kiro_passport_network_configure()` 会返回 `ESP_ERR_NOT_SUPPORTED`，设备无法安全保存 credential。必须先完成生产级 NVS encryption/flash encryption provisioning 方案，再经受控串口或工厂治具调用：

```c
kiro_passport_network_configure(
    "wss://ws.yanyunnnx.cc.cd",
    "<one-time-device-credential>");
```

启用 flash/NVS encryption 可能熔断 eFuse、影响后续刷写，是不可逆的硬件安全变更。请先在非生产设备上按 ESP-IDF 5.5 的官方 secure provisioning 流程验证并保留恢复/密钥托管方案；不要在现场 Wi-Fi 配网页输入 device credential。

设备满足 Wi-Fi 和可信 NTP 时间后，页面应依次显示 `Connecting relay` 与 `Relay ready`。Worker 收到的 WSS 路径应为：

```text
wss://ws.yanyunnnx.cc.cd/device/passport-AABBCCDDEEFF
```

## 6. Hook / Python 客户端联调

Worker 接受的 `tool` 为 1–31 个 `[A-Za-z0-9._:-]` 字符；`summary` 为 1–71 个不含双引号或反斜杠的可打印 ASCII 字符；TTL 为 1–300 秒（建议 60–120）。这是现有 ESP32 固件的严格 JSON parser 限制。

```python
import os
import time
import requests

BASE_URL = "https://ws.yanyunnnx.cc.cd"
DEVICE_ID = "passport-AABBCCDDEEFF"
HEADERS = {"Authorization": f"Bearer {os.environ['KIRO_PASSPORT_HOOK_TOKEN']}"}

created = requests.post(
    f"{BASE_URL}/v1/devices/{DEVICE_ID}/requests",
    headers={**HEADERS, "Content-Type": "application/json"},
    json={"tool": "shell.execute", "summary": "Run deployment command", "ttl_seconds": 60},
    timeout=10,
)
created.raise_for_status()
result = created.json()
request_id = result["request_id"]
deadline = time.monotonic() + 65

while result["status"] == "pending" and time.monotonic() < deadline:
    time.sleep(0.75)
    response = requests.get(f"{BASE_URL}/v1/requests/{request_id}", headers=HEADERS, timeout=10)
    response.raise_for_status()
    result = response.json()

if result.get("status") != "allow":
    raise PermissionError(f"Passport denied operation: {result}")
# Only execute the protected local operation here.
```

无论 HTTP 错误、`deny`、`offline`、`timeout`、`session_lost`、`protocol_error` 或客户端自身超时，都必须拒绝本地高风险操作。

## 7. 联调验收矩阵

1. **正常 allow**：设备连接为 `Relay ready`；创建请求；屏幕显示 tool/summary；按 OK；GET 最终返回 `{"status":"allow","reason":"user"}`。
2. **正常 deny**：按 DOWN；最终返回 `deny/user`；客户端绝不执行命令。
3. **离线**：断电/断 Wi-Fi 后 POST 应立即返回 `deny/offline`，不得产生 allow。
4. **超时**：不按键；到 `expires_at` 后 DO alarm 写入 `deny/timeout`，即使 WebSocket 空闲或 DO hibernate 也应成立。
5. **断线**：请求显示后断 Wi-Fi；该 session 的 pending 必须变为 `deny/session_lost`，重连不可恢复旧请求。
6. **错误身份或 credential**：WSS 握手应得到 401；不要把响应差异用作设备枚举接口。
7. **重放/篡改 decision**：修改 session ID、request ID、expiry 或重复传 decision 不得产生 allow；有效 pending 进入 `deny/protocol_error` 或保持 deny。
8. **轮换**：在 grace window 内新旧 credential 都能连接；到期后旧 credential 的存活 WSS 会话在下一次请求或 decision 时也必须进入 deny，不得产生 allow。
9. **撤销**：调用 `DELETE /v1/admin/devices/:device_id` 后，DO 关闭该设备 socket，未决请求为 `deny/session_lost`；此后 WSS 握手与任何已存活 socket 的 allow 均失败。

## 8. 当前固件兼容性说明

Worker 已发送 `decision_ack`，但当前 `main/kiro_passport_network.c` 会在 WebSocket 本地写成功后清除 user decision，尚未解析 ack。因此现阶段 DO 的持久化终态与 alarm 是正确性的兜底，但端到端的“设备确认 DO 已持久化”仍要等下一轮固件完成 ack/retry 状态机后才能满足。部署本 Worker 不会绕过这个限制。

Cloudflare 官方参考：

- [Durable Object WebSocket hibernation 示例](https://developers.cloudflare.com/durable-objects/examples/websocket-hibernation-server/)
- [Durable Object class migrations](https://developers.cloudflare.com/durable-objects/reference/durable-object-class-migrations-legacy/)
- [Durable Objects 概览](https://developers.cloudflare.com/durable-objects/)

Cloudflare 文档相关内容已概述和改写，以遵循许可限制。

## 9. Secure device-code enrollment

Device-code enrollment is for an unregistered `passport-<12 uppercase hex>` device. The device calls `POST /v1/enrollment/device-code` with `{"device_id":"passport-AABBCCDDEEFF"}`. The Worker returns a high-entropy `device_code`, a strict six-digit numeric `user_code` (display it as **6 digits**, with no separators), the `/admin/pair` URL, a 10-minute lifetime, and a required 5-second poll interval. Only peppered, purpose-separated SHA-256 hashes of both codes are stored in D1. The high-entropy device code, expiry, and poll-safety behavior are unchanged.

The device polls `POST /v1/enrollment/token` with `device_id` and `device_code`. While awaiting approval it receives `authorization_pending`; polling faster than the returned interval receives `slow_down`. Expired, denied, invalid, or already-consumed codes never return a credential. Once approved, the successful token exchange atomically creates the active `devices` record, consumes the enrollment, and returns the plaintext credential once. Do not log or persist that response outside encrypted device provisioning.

### Pairing operator workflow

`/admin/pair` is the only supported human-facing pairing entry point. After signing in through Cloudflare Access, the operator enters the six digits. The first POST only displays the matching pending `device_id`; it does not approve anything. The operator must then choose **Confirm bind** in a second POST. That confirmation is short-lived, signed with `PAIR_CONFIRMATION_SECRET`, and bound to both the Access subject and the selected enrollment. Invalid, expired, unavailable, or malformed entries return a safe, no-store HTML result without echoing raw input.

`GET /activate`, `POST /activate`, and JSON `POST /v1/enrollment/approve` remain compatibility APIs. Their POST approval calls still require a valid Access assertion and still approve directly. Do not direct new users to `/activate`.

### Cloudflare Access protection for pairing

Create a Cloudflare Access Application for the same Worker hostname with this protected path:

- `https://ws.yanyunnnx.cc.cd/admin/pair`

This one path covers the admin pairing page and both of its POST steps. The Worker independently verifies `Cf-Access-Jwt-Assertion` on every `/admin/pair` GET and POST, so a bypassed edge policy fails closed. Apply an allow policy limited to intended operators or an identity-provider group. You may add `https://ws.yanyunnnx.cc.cd/activate` to the same Application only when legacy browser activation remains required. Do **not** protect `/v1/enrollment/device-code` or `/v1/enrollment/token`: unregistered devices must reach those public device APIs, while Worker state transitions remain fail-closed.

Set the following Worker environment variables in the Cloudflare dashboard (or equivalent CI deployment configuration), per environment. They are configuration values, not credentials; do not substitute them with arbitrary user input:

- `ACCESS_TEAM_DOMAIN`: the exact Access team issuer, for example `https://your-team.cloudflareaccess.com` (the Worker accepts the hostname form and constructs HTTPS).
- `ACCESS_AUD`: the Application Audience (AUD) tag from that Access Application.

The Worker verifies assertions against the team JWKS using `jose`, requiring the configured issuer and audience. A missing or invalid assertion is rejected; approval audit identity stores both its email (`approved_by`) and subject (`approved_subject`). Pages use restrictive CSP, no-store caching, no referrer policy, and escaped output.

Keep `ADMIN_API_KEY`, `HOOK_AUTH_SECRET`, `DEVICE_CREDENTIAL_PEPPER`, `PAIR_CONFIRMATION_SECRET`, and generated device credentials as Worker secrets via `wrangler secret put`; never place them, Access assertions, or device/user codes in `wrangler.toml`, Git, CI logs, or source code. The enrollment schema is migrations `0002_device_enrollments.sql`, `0003_enrollment_approval_subject.sql`, and `0004_short_user_codes.sql`; after `npm run db:migrate:remote`, verify that `device_enrollments` is listed alongside the existing tables.
