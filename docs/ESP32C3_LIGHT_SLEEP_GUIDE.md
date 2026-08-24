# ESP32-C3 浅睡眠（Light Sleep）坑点与策略指南

> 从 AI Passport 通知器两轮功耗排查（2026-08-23/24，六层根因）中提炼的**可复用对策手册**。
> 完整排查叙事见 `AI_NOTIFY_POWER_DEBUG_SESSION.md`；本项目机制落点见
> `AI_NOTIFY_ARCHITECTURE.md` §4.6；浓缩运维条目见 `AI_NOTIFY_PLAYBOOK.md` 坑点 #12。
>
> **核心结论（TL;DR）**：配置全对 ≠ 在睡。sdkconfig 的 PM/TICKLESS 只是"上膛"，
> 运行时 `esp_pm_configure()` 才是"扣扳机"；扣了扳机还会被各种隐性锁"按住手"。
> 唯一铁证是 `esp_pm_dump_locks` 的锁时间占比与 SLEEP 统计，**不是** CPU duty。

---

## 1. 机制速览（30 秒版）

```
esp_pm 三态:  SLEEP ← APB_MIN ← APB_MAX ← CPU_MAX（能耗递增）
进入 SLEEP 条件: 无任何锁持有 + 连续空闲 ≥ CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP 个 tick
锁类型:
  ESP_PM_APB_FREQ_MAX   — APB 时钟必须开着(I2S/SPI/I2C/LEDC 传输窗口)
  ESP_PM_CPU_FREQ_MAX   — CPU 满频(计算密集窗口)
  ESP_PM_NO_LIGHT_SLEEP — 完全禁止浅睡眠(如 USB 调试保活)
BLE: modem sleep mode 1 与浅睡眠共存——控制器在连接事件间隙让射频休眠,
     LPCLK 用主晶振并保持供电(MAIN_XTAL_PU_DURING_LIGHT_SLEEP)
USB: 浅睡眠与 USJ 互斥——睡眠中错过主机 resume,端口即死
```

**关键认知：esp-idf 的外设驱动（I2S/SPI/I2C 等）初始化/enable 期间会自动持有
`APB_FREQ_MAX` 锁。外设"初始化后常驻" = 锁常驻 = 浅睡眠被静默禁止，且没有任何
警告日志。**

## 2. 坑点清单（按踩坑顺序，症状 → 根因 → 判定 → 修复）

### 坑 1：配置上膛但没扣扳机

| | |
| --- | --- |
| 症状 | sdkconfig 配齐 PM/TICKLESS/modem sleep，启动日志正常，整夜 ~10%/h 级耗电 |
| 根因 | `esp_pm_configure()` 从未调用——`s_light_sleep_en` 默认 false，sdkconfig 只提供机制 |
| 判定 | `esp_pm_dump_locks` 输出里 SLEEP 0% 且无锁持有；或 duty 与全开状态相同 |
| 修复 | 启动代码显式 `esp_pm_configure(&cfg)`（`light_sleep_enable=true`） |

### 坑 2：任务空转饿死 tickless

| | |
| --- | --- |
| 症状 | 无锁持有但 `light_sleep_counts:0` |
| 根因 | 某任务循环末尾无条件 `vTaskDelay(1)`（esp_lvgl_port 1ms 空转）——tickless 需连续空闲 ≥3 tick，每 1ms 醒一次永远凑不满 |
| 判定 | PM dump 的 reject_counts=0 但 SLEEP=0%（没尝试也没拒绝=空闲凑不齐） |
| 修复 | 空闲期**挂起整个任务**（不是只停它的定时器）。⚠ 挂起可能持互斥锁的任务必须**持锁挂起**（挂起瞬间保证不在临界区，否则锁被挂起任务持有 → 后续上锁永久死锁） |

### 坑 3：射频域浅睡不断电

| | |
| --- | --- |
| 症状 | SLEEP 占比正常但耗电仍高 |
| 根因 | `CONFIG_ESP_PHY_MAC_BB_PD` 未启用，浅睡眠期间 MAC/BB 域仍供电 |
| 判定 | 电流测量（浅睡眠理论 ~130µA 数字域 + 射频域残余） |
| 修复 | 启用（官方 c3/40M power_save 示例标配，RAM +2KB） |

### 坑 4：USB 悬空噪声攻破保活锁（本项目第四层）

| | |
| --- | --- |
| 症状 | 拔 USB 后浅睡眠仍 0%，`usb_awake` 类锁 100% 常驻 |
| 根因 | D+/D- 悬空后被电气噪声周期性误报"已连接"（实测 1h 误报 ~95 次、可产生 ≥5s 连续假象）→ 任何依赖 `usb_serial_jtag_is_connected()` 的续期逻辑被噪声永久续命 |
| 判定 | PM dump：NO_LIGHT_SLEEP 锁 Time=100% + 状态日志里连接/断开高频抖动 |
| 修复 | ① 不启用 `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`；② 保活锁只做**上电一次性宽限**（如 10 分钟覆盖烧录-调试节奏），到期释放后**不依赖任何运行时 USB 状态续期**——任何去抖阈值都会被噪声攻破 |

### 坑 5：外设驱动常开 = 隐性 PM 锁（本项目第六层，最隐蔽）

| | |
| --- | --- |
| 症状 | 修完前四层，radio-off 验证 SLEEP 74%，但生产固件耗电纹丝不动（~10%/h） |
| 根因 | 播放器启动时打开 codec 后常驻 → I2S 通道 enable → **esp-idf I2S 驱动自动持 `APB_FREQ_MAX` 锁且永不释放**。连接态/广播态同样受害，与射频状态无关 |
| 判定 | PM dump：`i2s_driver APB_FREQ_MAX` 两把锁 100% 常驻、`light_sleep_counts:0` |
| 修复 | **用完即关**：使用窗口 open，结束立即 close/disable（本项目：播放前 `set_format`、播完 `bsp_audio_close`）。⚠ esp_codec_dev 重开坑：`esp_codec_dev_open` 内部会 disable 通道（要求 RUNNING），通道在 READY 态时需预 enable 一次让它合法 |
| 泛化 | 任何"初始化一次永久使用"的外设（SPI 屏、I2C 传感器、摄像头）都应审视：不用时能否 close？关不掉的成本要显式记入功耗预算 |

### 坑 6：cpu duty 是 WFI 假象（两轮排查都被它骗）

| | |
| --- | --- |
| 症状 | duty ~5% 看起来"睡得很好"，电流却是 20mA 级 |
| 根因 | **mcycle 在 WFI（醒着等中断）时也停走**——"醒着空闲"与"真睡眠"的 duty 基线几乎相同（实测都在 5~6%） |
| 判定 | duty 必须配合"必然清醒"的对照窗口（如 USB 宽限期内）——两者相同即没睡。**终裁只能靠 PM dump 锁统计或电池斜率** |
| 修复 | 不用 duty 做结论；把 `esp_pm_dump_locks` 挂在 60s 心跳处（`#ifdef CONFIG_PM_PROFILING` 保护，正式版零开销） |

### 坑 7：USB 观测死锁（要看睡眠行为 vs 串口观测互斥）

| | |
| --- | --- |
| 症状 | 想看睡眠日志，但 USB 一睡就断 |
| 根因 | 浅睡眠中 USJ 无法响应主机 resume |
| 破解 | ① PM dump 统计是开机累计——拔线（睡）→ 重插（不复位）→ 立刻读数，历史不丢；② 短暂浅睡眠（几十 ms 级）不断 USB 流，串口仍可读；③ **反向探针**：宽限过后 USB 端口消失 = 浅睡眠真发生（WFI 杀不死 USB）——"故障现象"变证据 |
| 运维 | 端口睡死后的恢复：按键唤醒（芯片清醒）→ 拔插 USB |

### 坑 8：短窗斜率测量被噪声淹没

| | |
| --- | --- |
| 症状 | 20 分钟窗口三次"同条件"测量斜率 6/12/11%/h 互相打架 |
| 根因 | 充电后电压弛豫（几十 mV 缓降伪装负载）+ SOC 1% 量化噪声 |
| 修复 | <2h 窗口只配冒烟不配结论；结论用 ≥1h 窗口 + 过夜记录（每 5min 带日期采样）；满电平台期（~100%）电压平坦，精确斜率要在电量中段测 |

### 坑 9：验证场景与生产形态不一致（第六层漏网的元凶）

| | |
| --- | --- |
| 症状 | radio-off 诊断构建验证 SLEEP 74% "通过"，生产固件从未睡过 |
| 根因 | 诊断构建恰好没跑音频——I2S 锁不存在于验证场景，存在于生产场景 |
| 修复 | **验证场景必须等价于生产形态**（音频开/关、USB 插/拔、连接/广播逐一对照）。二分实验的"无变化"结论要复查掩蔽变量（参见坑 4 掩蔽坑 5 的历史） |

## 3. 策略清单（设计期 checklist）

### 3.1 锁持有者盘点表（低功耗设计的第一件事）

为系统中每一个 `esp_pm` 锁建立一行记录，明确**谁、什么窗口、什么类型**：

| 锁名 | 类型 | 持有窗口 | 释放点 |
| --- | --- | --- | --- |
| screen | APB_FREQ_MAX | 亮屏期间 | 息屏序列末尾 |
| play | APB_FREQ_MAX | 播放期间（含 codec open/close） | 播完立即 |
| i2c | APB_FREQ_MAX | I2C 传输期间（电池采样） | 采样结束 |
| usb_awake | NO_LIGHT_SLEEP | 上电一次性宽限 | 到期释放，永不续期 |
| （驱动隐性锁） | — | **逐个外设审计：不用时是否 close？** | — |

设计原则：
- **用完即关**：外设的使用窗口结束就 close（显式释放驱动锁）
- **宽限一次性**：调试保活类锁只做上电宽限，不依赖运行时状态续期（见坑 4）
- **持锁挂起**：挂起可能持互斥锁的任务前先取锁（见坑 2）
- 锁创建失败只降级省电，不阻塞功能

### 3.2 射频侧功耗结构

| 状态 | 射频占空比 | 设计策略 |
| --- | --- | --- |
| 连接态（15~30ms 间隔，latency 0） | ~5~10% | 可协商慢连接参数（50ms+latency4 再降 5~10 倍，注意与主机重连行为的兼容性泡测） |
| 快广播（100~150ms） | ~1.2% | 只用于断连后的短窗（~5 分钟）等重连 |
| 慢广播（1~1.5s） | ~0.15% | 长期待命档；主机扫描超时要 ≥ 8s 保证命中 |
| 分级广播状态机 | — | 断连/开机 → 快档 → N 分钟无人连 → callout 降慢档；连接成功重置快档 |

**断连后无限期快广播等主机 = 空耗**，是低功耗 BLE 外设的常见反模式。

### 3.3 息屏序列（屏幕类外设的通用模式）

```
息屏: 背光 PWM 归零 → 挂起 UI 任务 → 面板进睡眠(SLPIN,GRAM 保持) → 释放 screen 锁
亮屏: 按 screen 锁 → 面板唤醒(SLPOUT,内置 ~120ms 延时) → 恢复 UI 任务(补画脏区) → 背光
```

只关背光不睡面板 = 漏 1~3mA；只停 UI 的 tick 不挂起任务 = 坑 2。

## 4. 验证方法论（分阶段，成本递增）

| 阶段 | 手段 | 结论强度 |
| --- | --- | --- |
| 0. 仪器标定 | 短时深睡对比掉电 | 一次性验证电量计/电池可信度，**任何功耗排查的第一步** |
| 1. 冒烟 | 60s duty 心跳日志 | 仅参考（坑 6：WFI 假象），须配清醒对照窗口 |
| 2. 定罪 | `CONFIG_PM_PROFILING` + `esp_pm_dump_locks(stdout)` | **锁时间占比 + SLEEP% + 入睡/拒绝计数 = 唯一铁证** |
| 3. 行为探针 | 宽限后 USB 端口死活 | 端口消失 = 真睡了（反向证据，成本为零） |
| 4. 事后取证 | status 协议的 `cyc` 字段（32 位 mcycle 快照）差值分析 | 从历史日志离线算各时段 duty；注意 2^32 回绕处理 |
| 5. 终审 | ≥1h 电池斜率 + 过夜记录（每 5min 采样） | 排除短窗噪声；电量中段测；剔除断连重连风暴窗口 |

**duty 离线算法**：`Δcyc（处理回绕）÷ (Δt × 160MHz)`；满速清醒基线 ~5%，
显著低于此且无锁持有 → 睡了；与清醒基线相同 → 没睡。

## 5. 故障速查表

| 症状 | 首查 | 对应坑 |
| --- | --- | --- |
| 耗电高 + PM dump 无锁 + SLEEP 0% + reject 0% | 找空转任务 | 坑 2 |
| 耗电高 + 某锁 Time=100% | 锁的持有者是谁？为何不释放？ | 坑 4/5 |
| duty 正常但耗电高 | 别信 duty，看 dump | 坑 6 |
| radio-off 验证通过、生产固件不睡 | 验证场景与生产形态的差异点 | 坑 9 |
| SLEEP% 正常但耗电仍高 | 射频域/外设静态电流（MAC_BB_PD、PA 常通、面板） | 坑 3 + §3.3 |
| 短窗测量结果互相打架 | 测量窗口噪声 | 坑 8 |
| USB 一睡就断看不到日志 | 累积统计 + 拔插不复位 + 反向探针 | 坑 7 |

## 6. 本项目最终配置快照

```ini
# sdkconfig.defaults 电源管理段
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
CONFIG_BT_CTRL_MODEM_SLEEP=y
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y
CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y
CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y
CONFIG_ESP_PHY_MAC_BB_PD=y
CONFIG_BUTTON_PERIOD_TIME_MS=20        # 按键轮询 5→20ms
# 不启用 CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION(悬空噪声会饿死浅睡眠,见坑 4)
```

代码侧：启动 `esp_pm_configure` → 锁全景见 §3.1 表 → 播放/息屏生命周期见
`AI_NOTIFY_ARCHITECTURE.md` §4.6（含每次改动的完整 rationale）。
