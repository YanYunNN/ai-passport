# Kiro Passport Relay 前端样式规范（Web UI Style Guide）

> 适用对象：`cloudflare/` Worker 下的全部网页 —— 管理控制台 `/admin`（含配对/激活等共用页）、语音助手 `/voice`、在线模拟器 `/simulator`。
>
> 视觉参考：Slash 设计系统（refero.styles.design 收录的 "Midnight vault with gilded ledger lines"）。2026-09-03 全站按此重构落地，本文件是唯一权威的样式规范，改样式前先读这里。

## 0. 一句话总纲

午夜近黑画布 + 灰阶表面栈 + 发丝级 1px 边框（**不用投影**）+ 单一暖铜色点缀 + 白色只给「每屏唯一主操作」。

---

## 1. 代码位置地图（单一事实来源）

| 文件 | 内容 |
|---|---|
| `cloudflare/src/ui.ts` | 导出 `UI_CSS`：**全站唯一 token 定义处**（`:root` 变量、reset、scrollbar、selection、focus、reduced-motion）。改配色只许改这里 |
| `cloudflare/src/index.ts` | `ADMIN_PAGE_CSS`：管理控制台 6 个 Tab、配对/激活/绑定等所有走 `htmlPage()` 的页面 |
| `cloudflare/src/voice.ts` | `VOICE_PAGE_CSS`：语音助手聊天页 |
| `cloudflare/src/simulator.ts` | `SIM_PAGE_CSS`：Web 模拟器 / 在线烧录页 |

每个页面的 `<style>` 都是这样拼的（页面专属样式只写本页增量，禁止重复定义 token）：

```html
<style>${UI_CSS}
${ADMIN_PAGE_CSS}</style>   <!-- 其它页面同理替换为各自 PAGE_CSS -->
```

> 新增页面三步：① `import { UI_CSS } from "./ui"`；② 只写本页的 `XXX_PAGE_CSS`（颜色一律引用 token）；③ 在 `index.ts` 的 fetch 路由注册。

---

## 2. 核心原则（Do / Don't）

### Do ✅
- 分层用灰阶表面 + 1px 边框表达层级，**替代阴影**。
- Copper `#cc9166` 是调色板里**唯一**的彩色音符：用于分类标签/eyebrow、导航激活下划线、编辑性链接、待办/进行中状态。
- 白色实心按钮（`.btn-primary`）是稀缺资源：**一个可视区域内最多一个**，只给最重要操作。
- 状态色只用规范里的 muted 语义色（ok/warn/err），并保持低饱和。
- 图表/进度条等数据可视化用金色系（`--gold`/`--gold-hi`）。
- 正文一律 `--bone`，不用纯白大段文字；纯白留给标题/主按钮。
- 按钮、输入框、标签、徽章、状态点用胶囊 `9999px`；卡片/面板 `10px`。

### Don't ❌
- 不要重新引入蓝/绿/青/紫（indigo/violet/cyan 系列曾在旧版存在，已全部移除）作为品牌装饰。
- 不要给卡片/弹层加 drop-shadow 光晕；悬浮反馈用「边框变亮 + 背景微亮」。
- 正文/界面文字不要用旧 GitHub 霓虹绿 `#3fb950`、霓虹红 `#f85149` 这类高饱和值。
- 不要在页面 CSS 里自造 hex 常量 —— 一律引用 token 或别名。
- 展示文案不要用纯白 `#ffffff` 长文；图标容器不要发光。

---

## 3. 设计 Token（与 `ui.ts` 逐字一致）

### 3.1 画布与表面栈（灰阶）

| Token | 值 | 用途 |
|---|---|---|
| `--void` | `#08080a` | 页面画布 |
| `--onyx` | `#040406` | 卡片表面（视觉上比画布略"凹"进去） |
| `--carbon` | `#121317` | 面板：输入框、弹层、代码块、popover |
| `--graphite` | `#1c1d22` | 主边框 / 分隔线 |
| `--slate` | `#2e3038` | 强边框、hover 行、设备外壳描边 |
| `--smoke` | `#464853` | 三级边框 |

### 3.2 文字阶梯（值越小越弱）

| Token | 值 | 用途 |
|---|---|---|
| `--ash` | `#5e616e` | 最弱元数据 / footer |
| `--steel` | `#777a88` | 弱正文、占位、标签 |
| `--fog` | `#9194a1` | 次级正文、说明文字 |
| `--mist` | `#acafb9` | 次级内容（表格数据等） |
| `--silver` | `#c7c9d1` | 强调数据标签 |
| `--bone` | `#e2e3e9` | **默认正文** |
| `--white` | `#ffffff` | 稀缺：主按钮 / 页面大标题 / 用户气泡 |

### 3.3 强调色与语义状态

| Token | 值 | 用途 |
|---|---|---|
| `--copper` / `--copper-hi` | `#cc9166` / `#e0b28c` | 唯一品牌色 / 其亮阶（hover、浅底文字） |
| `--copper-soft` | `rgba(204,145,102,0.13)` | copper 系背景（notice、激活底） |
| `--ok`（+`--ok-bg`/`--ok-line`） | `#7ecf9f` | 在线/成功/已送达 |
| `--warn`（+`--warn-bg`/`--warn-line`） | `#d9b98a` | 离线/警告/待重试 |
| `--err`（+`--err-bg`/`--err-line`） | `#eb9090` | 失败/吊销/错误 |
| `--gold` / `--gold-hi` | `#ae9357` / `#d0b478` | **仅限数据可视化**：折线、面积渐变、进度条 |

> 状态三元组（bg/line）固定格式：`rgba(<主色RGB>, 0.12)` 做背景、`0.3` 做边框。

### 3.4 形状与字体

| Token | 值 | 用途 |
|---|---|---|
| `--r-card` | `10px` | 卡片、面板、输入块 |
| `--r-pill` | `9999px` | 按钮、输入框、徽章、状态点、子 Tab |
| `--font-ui` | Inter → 系统栈（含 PingFang SC / Microsoft YaHei） | 全站界面 |
| `--font-mono` | ui-monospace / SF Mono / Consolas 栈 | 代码、设备 ID、时间戳 |

### 3.5 兼容别名（重要约定）

旧模板与内联 JS 里的 `var(--bg)`、`var(--card-bg)`、`var(--border)`、`var(--text-muted)`、`var(--accent)`、`var(--accent2)`、`var(--online)`/`var(--offline)`、`var(--panel)`、simulator 的 `var(--bg-card)` 等，全部在 `:root` 里映射到了新 palette（见 `ui.ts` `legacy aliases` 段）。

**约定：新代码一律写规范名；别名只用于兼容既有内联样式，不在新代码里使用。** 内联 JS 设状态色请用 `var(--ok)` / `var(--warn)` / `var(--err)`。

---

## 4. 排版

| 角色 | 字号/字重 | 备注 |
|---|---|---|
| 页面 H1 `.page-head h1` | `1.55rem` / 650 | 白色，`letter-spacing:-0.02em` |
| 卡片标题 `.card-title` | `1rem` / 600 | `--bone` |
| 正文 | `14–15px` / 400 | 行高 1.5，`--bone` |
| 次级说明 `.desc/.page-desc` | `0.88rem` | `--fog` |
| 栏目小标 `.section-title` | `0.72rem` / 600 | 大写 + `0.1em` 字距，`--steel`（铜色 eyebrow 仅在确属"分类/标签"语义时使用） |
| 表头 `th` | `0.68rem` / 600 | 大写 + 字距，`--steel` |
| 数值 | — | `font-variant-numeric: tabular-nums` |
| 代码/ID `.code-mono` | `0.86em` | `--font-mono`，`--carbon` 底 + `--graphite` 边 |

---

## 5. 组件规范

### 5.1 顶栏与导航

| 元素 | 规范 |
|---|---|
| `.topnav` | sticky、`rgba(8,8,10,0.78)` + `blur(18px)`、底边 `--graphite` |
| `.brand` | `--bone` 普通；`span` 内主名用 `--white` 700。**禁止渐变文字** |
| `.nav-tab` | 透明底、`--steel` 文字；hover 文字 `--bone` + 微亮底 |
| `.nav-tab.active` | 文字 `--bone`，**2px 铜色底线下划线**（`box-shadow: inset 0 -2px 0 var(--copper)`），不用填充块 |
| `.sub-tab` | 胶囊分段控件：`--graphite` 边框；active = 微亮底 + `--white` |
| `.nav-btn` | 顶栏右侧胶囊 ghost 按钮 |

### 5.2 卡片

| 元素 | 规范 |
|---|---|
| `.card` / `.stat-card` / `.quick-card` / `.card-box` | 底 `--onyx`、边 `--graphite`、圆角 `--r-card`、**无投影**；hover 边框升到 `--slate` |
| `.card-header` | 与正文用 1px `--graphite` 分隔 |
| `.stat-icon` / `.quick-icon` | emoji 置于 `--carbon` 圆形胶囊中（直径 38–42px），不发光 |
| `.stat-value` | `1.9rem` / 600 / 白 / tabular-nums |

### 5.3 按钮层级（重要）

| Class | 形态 | 使用 |
|---|---|---|
| `.btn-primary` | 白色实心胶囊、深色文字 | **唯一主操作**，每屏 ≤1 |
| `.btn-accent` / `.btn-secondary` | 透明 + 1px 半透明白/灰边框 ghost | 次要操作 |
| `.btn-danger` | `--err` 文字 + 边框 | 危险操作 |
| `.btn-sm` / `.btn-block` | 小号 / 通栏变体 | — |

hover 一律「背景微亮 / 边框升一级」，**不加发光阴影**；`:focus-visible` 用 2px `--copper` outline。

### 5.4 表单与徽章

- `.form-input` / `.form-select` / `.textinput`：底 `--carbon`、边 `--graphite`、**胶囊圆角**；focus 边框 `--copper` + 3px `rgba(204,145,102,0.12)` ring。
- `.badge`：胶囊、11–12px、边框 1px；`.badge-active`=ok 组、`.badge-revoked`=err 组；"进行中/待审批"用 copper 组。
- `.dot-online`（ok 组，弱呼吸圈）/ `.dot-offline`（`--ash` 静态灰点）。
- `.code-input`（配对码）：`--carbon` 底、等宽字体、`letter-spacing:0.2em` 居中。

### 5.5 表格与日志

- `th`：透明底、大写小字距标签；`td` 用 `--mist`，行间 1px `rgba(226,227,233,0.07)` 分隔；`tr:hover` 仅微亮（不换底色块）。
- `.hook-log-table`：紧凑行距（`0.68rem 0.85rem`）；设备/时间/状态列不折行，标题/内容列 `.cell-clamp` 截断，外层 `.log-table-scroll` 兜底横向滚动。
- 长文本列用 `.cell-clamp`（2 行）/ `.cell-clamp-1`（1 行）+ `title` 悬浮全文。

### 5.6 监控与图表（金线规则）

- 图表线/面积渐变只允许金色：线 `--gold-hi`，面积渐变 `rgba(208,180,120,0.3) → transparent`；网格用中性灰 `rgba(226,227,233,0.08)`。
- `.mon-uptime-fill`：金色渐变；`.mon-event-online/offline` 用 ok/err 组徽章；在线率百分比 `--steel` + tabular-nums。
- 屏幕预览 `.screen-frame`：`#000` 画布 + `--slate` 边框，允许**极淡**暗投影模拟立体感，禁止彩色 glow。

### 5.7 语音页（voice.ts）

- `.bubble.user`：**白色气泡 + 深色文字**（用户话语 = 稀缺高对比）；`.bubble.ai`：`--carbon` 底 + `--graphite` 边。
- `.micbtn`：白色圆形（页面唯一主操作）；录音态切 `--err` 红 + 弱呼吸圈。
- `.speakbtn.playing/.paused`：copper / warn 组；`.cfgbtn`、`.logbtn`、`.sendbtn` 为 ghost 胶囊；`.cfgval` 数值用 `--copper-hi`。
- 弹层 `.cfgpanel`/`.logpanel`：`--carbon` 底 + `--slate` 边，允许适度暗投影（悬浮层例外）。

### 5.8 模拟器页（simulator.ts）

- 设备机身 `.device-wrapper`：中性石墨渐变 `var(--device-casing)` + `--device-border`，**不是 UI 层**，可保留自身物理质感。
- LED：ok/warn/err 语义组；按键 `.hw-btn` 石墨键帽，按压态边框升 copper。
- `.tab-btn`（模拟器子页签）：与 `.nav-tab` 同一激活语言（铜色下划线）。
- `.card-box`/`.form-control`/`.btn-*`/`.dropzone`/`.preset-card`/`.preset-badge`：同 5.2–5.4 规范；拖放高亮边框 copper；烧录进度条金渐变。
- 终端（xterm）内保留 ANSI 语义色但换 muted 版本（见代码），其配色属于"模拟设备内容"，改动不受本规范 UI 层约束。

---

## 6. 布局与密度

| 页面 | 主容器宽 | 密度 |
|---|---|---|
| 管理控制台 | `max-width: 1120px`，`.container` 顶部留白 `2.25rem` | compact |
| 语音页 | 聊天区 `720px` 居中 | compact |
| 模拟器 | `grid 480px + 1fr`，最大 `1540px` | compact |

- 画布是**连续的单色 `#08080a`**，分段靠卡片/边框，不靠交替底色；允许顶部一处极淡 radial 暖光（铜色 alpha ≤ 0.05）。
- 卡片组间距 `1rem–2rem`；元素间距基准 `4/8px` 倍数；footer 居中 `--ash` 小字。

---

## 7. 工程红线（本仓库特有，必读）

1. **CSS/JS 全部写在 TS 模板字符串里**：写 CSS/HTML 内容时禁止出现 `` ` ``、`${`、反斜杠 `\`（会破坏模板或产出坏正则/坏字符串）。
2. 内嵌页面 `<script>` 的字符串里不能出现字面 `</script>`。
3. 旧 JS 内联颜色历史上直接写 hex；**新代码一律用 token/别名**，状态用 `var(--ok|--warn|--err)`，图表用 `--gold` 系。不要在 JS 里再造霓虹色。
4. 校验流程（无法起本地 UI 时也至少做前三项）：
   ```sh
   cd cloudflare
   npx tsc --noEmit                                   # 类型 + 模板字符串完整性
   npx esbuild src/index.ts --bundle --external:cloudflare:workers --outfile=/tmp/x.mjs
   # 结构核对：HTML/JS 里用到的 class 都应在对应 PAGE_CSS 中有定义
   npx wrangler dev --local                            # 起本地渲染抽查页面
   ```
5. 部署**必须** `npx wrangler deploy --keep-vars`（裸 deploy / `npm run deploy` 会清掉仪表盘远程 vars）。
6. 上线前自查：无旧靛蓝/青/紫/霓虹色残留（grep `#6366f1|#58a6ff|#3fb950|#4f46e5|#22d3ee|#1f6feb`）；`/admin`、`/voice` 仍返回 401（Basic Auth 未受影响）；新样式 token 出现在页面源码里。

---

## 8. 新增页面前端清单

- [ ] `import { UI_CSS } from "./ui"`，`<style>` 只写 `${UI_CSS}` + 本页 CSS
- [ ] 颜色引用 token；按钮按 5.3 层级（白色主按钮每屏 ≤1）
- [ ] 路由注册进 `index.ts` fetch（带鉴权检查，admin 系复用 `verifyAdminBasicAuth`）
- [ ] 页面文案与整体页面一致（`lang="zh-CN"`）；emoji 作为图形元素允许
- [ ] `npx tsc --noEmit` 通过，页面渲染无 `${UI_CSS}` 字面残留
- [ ] 部署：`npx wrangler deploy --keep-vars`
