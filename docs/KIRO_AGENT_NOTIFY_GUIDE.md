# Kiro Agent 完成通知 → AI Passport 原生文本提示

当 Kiro agent 结束一轮（`Stop` hook 触发）时，把它本条产出的消息摘要作为**原生文本通知**推到 AI Passport 设备上屏展示。

## 链路总览

```text
Kiro agent 完成(Stop hook)
  -> .kiro/hooks/kiro-passport-session-idle.json  (已启用)
  -> python tools/kiro_passport_bridge.py hook --kind idle
       (读取 hook stdin 中的 agent 最终文本)
  -> POST https://ws.yanyun.asia/v1/devices/:id/notify   (HOOK_AUTH_SECRET 鉴权)
  -> Cloudflare Worker -> PassportRelay Durable Object
  -> WSS 下发 {"v":1,"type":"notify", ..., "content": "<agent 文本>"}
  -> 固件 process_message 解析 notify -> 存入通知缓冲区
  -> "Kiro Passport" 页面显示 title + content，OK 长按/短按关闭
```

## 2. 新增/变更点（本功能涉及的全部改动）

| 层 | 文件 | 变更 |
|---|---|---|
| 固件 | `main/kiro_passport_network.c` | `process_message` 识别 `"type":"notify"`，解析 `id/title/content`，存入静态通知缓冲区 |
| 固件 | `main/kiro_passport_network.h` | 新增 `kiro_passport_network_get_notify()` / `kiro_passport_network_clear_notify()` |
| 固件 | `main/demo_kiro_passport.c` | 有通知时在 Kiro 页展示标题+内容；OK 短按关闭 |
| Worker | `cloudflare/src/protocol.ts` | 新增 `DeviceNotify` + `serializeDeviceNotify()`（≤1000 字节、可打印 ASCII） |
| Worker | `cloudflare/src/index.ts` | 新增 `POST /v1/devices/:id/notify`（`HOOK_AUTH_SECRET` 鉴权） |
| Worker | `cloudflare/src/passport-relay.ts` | 新增 `/internal/notify` 透传 |
| 工具 | `tools/kiro_passport_bridge.py` | `hook --kind idle` 读取 agent 文本并 POST notify；新增 `notify` 子命令 |
| Hook | `.kiro/hooks/kiro-passport-session-idle.json` | 启用 `Stop` hook |

**新增协议帧**（`cloudflare` → 设备，单条文本，≤1000 字节）：

```json
{"v":1,"type":"notify","device_id":"passport-XXXXXXXXXXXX",
 "session_id":"<32hex>","id":"<uuid>","title":"Agent done",
 "content":"<可打印 ASCII，≤~900>","ts":1234567890}
```

所有字段为可打印 ASCII（`0x20-0x7E`，不含 `"` `\`），与固件现有解析器同源安全约束。`content` 若超长，Worker 侧会以 `...` 截断到整帧 ≤1000 字节。

## 3. 部署 Worker（一次性）

见 `cloudflare/DEPLOYMENT.md` 的完整流程；本功能涉及的关键步骤：

```sh
cd cloudflare
npm install
npx wrangler login

# 首次：创建 D1 并在 wrangler.toml 填 database_id，再 apply migration
npx wrangler d1 create kiro-passport
npm run db:migrate:remote

# 写入 secrets（交互式输入，勿贴进命令行历史）
npx wrangler secret put HOOK_AUTH_SECRET
npx wrangler secret put ADMIN_API_KEY
npx wrangler secret put DEVICE_CREDENTIAL_PEPPER
npx wrangler secret put ADMIN_UI_USERNAME
npx wrangler secret put ADMIN_UI_PASSWORD

# 部署
npx wrangler deploy
```

> 提示：仅如果之前已注册过设备，不需要再次注册。若设备尚未注册，先在 `/admin/pair` 完成一次配对。

### 验证 notify 端点（云端，不由实体设备）

```sh
curl -X POST 'https://ws.yanyun.asia/v1/devices/passport-XXXXXXXXXXXX/notify' \
  -H "Authorization: Bearer $HOOK_AUTH_SECRET" \
  -H 'Content-Type: application/json' \
  -d '{"title":"test","content":"hello from host"}'
# 预期: {"ok":true,"sent":true,"online":true} (设备在线) 或 sent:false (离线)
```

## 4. 配置本机 bridge

`.kiro/passport_config.json`（被 gitignore，需手动）写入：

```json
{
  "relay_url": "https://ws.yanyun.asia",
  "device_id": "passport-XXXXXXXXXXXXXXXX",
  "hook_token": "<与 HOOK_AUTH_SECRET 相同的值>"
}
```

等价命令：

```sh
python3 tools/kiro_passport_bridge.py config --device-id "passport-<ID>"
python3 tools/kiro_passport_bridge.py config --token "$HOOK_AUTH_SECRET"
python3 tools/kiro_passport_bridge.py config --relay-url "https://ws.yanyun.asia"
```

检查桥接配置与云端健康：

```sh
python3 tools/kiro_passport_bridge.py status
```

## 5. Kiro hook（已启用 Stop idle hook）

`.kiro/hooks/kiro-passport-session-idle.json` 已把 `enabled` 置为 `true`，触发为 `Stop`（agent 完成一轮回复时）：

```json
{ "action": { "type": "command",
    "command": "python3 tools/kiro_passport_bridge.py hook --kind idle", "timeout": 10 },
  "enabled": true }
```

bridge 会读取 hook stdin 的 JSON payload，按优先级取 `final_assistant_message / assistant_message / message / text / content / output / answer / summary / last_message` 等字段；无法提取时回退到固定文本 `Agent finished.`。

> 同一会话每轮 `Stop` 都会触发通知；若你只想在**整条对话结束时**提示一次，请开启调用方/脚本侧的去抖（本仓库不为此新增逻辑）。

## 6. 烧录固件并在真机验证（物理介入）

1. 编译：

   ```sh
   get_idf553
   idf.py set-target esp32c3   # 仅全新 checkout
   idf.py build
   ```

2. 生成（或复用 `deploy/` 流程）all-in-one BIN 并烧录（**不要勾选 Erase all**，保留 NVS；首次该布局需重新配网）：

   ```sh
   idf.py flash monitor
   ```

   或用 `deploy/FoloToy-AI-Passport_0x0_all-in-one.bin`（见 `deploy/FLASHING.md`）。

3. 设备连接到 Wi-Fi、完成信同步后，应显示 relay **在线**。

4. 真实验收清单（逐条勾选）：

   - [ ] 设备连接 `wss://ws.yanyun.asia/device/<id>`，Relay 状态为 Online。
   - [ ] `curl ... /notify` 返回 `"sent":true,"online":true`。
   - [ ] 设备切到「Kiro Passport」页面，出现刚才 push 的 `title` 与 `content` 文本（自动换行）。
   - [ ] 按 **OK 短按**，通知清除，页面回到默认 `High-risk Kiro tools appear here.`。
   - [ ] 断开 relay / 设备离线时 `notify` 返回 `sent:false`，且设备不受影响。
   - [ ] 在 Kiro 里让 agent 完成一轮回复（触发 `Stop`），观察设备收到 agent 最终文本。
   - [ ] 审批流程（高风险 tool 的 allow/deny）未被本功能破坏：验证一次鼠标 OK/DOWN 通过。

## 7. 常见问题

| 症状 | 原因/处理 |
|---|---|
| 设备不显示任何 notify | 设备不在「Kiro Passport」页面 / 通知版本未刷新；确认 `notify` API 返回 `sent:true` 且设备在线 |
| `notify` API 401 | `hook_token` 未配置或与 `HOOK_AUTH_SECRET` 不一致；`tools/kiro_passport_bridge.py config --token` |
| `sent:false`/`online:false` | 设备离线；检查 Wi-Fi、NTP、WSS 连接 |
| 中文内容乱码 | 固件 notify 仅支持可打印 ASCII，超出部分被替换为空格；agent 文本需 ASCII（或后续升级 UTF-8） |
| hook 不触发 | 检查 Kiro `Stop` hook 是否被启用、bridge 能否运行（`python3 tools/kiro_passport_bridge.py hook --kind idle`） |