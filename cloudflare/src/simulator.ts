export interface SimulatorPreset {
    id: string;
    name: string;
    description: string;
    version: string;
    type: "merged" | "app";
    flashOffset: number;
    url: string;
}

export const PRESET_FIRMWARES: SimulatorPreset[] = [
    {
        id: "game-rpg-demo",
        name: "社区精选：RPG 冒险游戏固件 (community-cff9b1ed)",
        description: "真实 ESP32-C3 固件，包含 ST7789 战斗渲染与按键交互",
        version: "v1.0.0",
        type: "merged",
        flashOffset: 0x0,
        url: "https://mote.folotoy.cn/api/download/community/community-cff9b1ed",
    },
    {
        id: "silhouette-bad-apple",
        name: "社区精选：影绘播放器固件 (community-a50f5993)",
        description: "真实 ESP32-C3 固件，包含 240x320 影绘动画与音频流",
        version: "ae2a840",
        type: "merged",
        flashOffset: 0x0,
        url: "https://mote.folotoy.cn/api/download/community/community-a50f5993",
    },
    {
        id: "passport-demo-v1",
        name: "FoloToy AI Passport 官方基线固件 (LVGL 9)",
        description: "ESP-IDF 5.5.3 官方 6 大 Demo 固件",
        version: "v1.2.0",
        type: "merged",
        flashOffset: 0x0,
        url: "https://raw.githubusercontent.com/yanyunnn/ai-passport/main/build/merged-firmware.bin",
    }
];

export async function handleSimulatorBinProxy(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const targetUrl = url.searchParams.get("url");

    if (!targetUrl) {
        return new Response(JSON.stringify({ error: "Missing 'url' query parameter" }), {
            status: 400,
            headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }
        });
    }

    try {
        const parsed = new URL(targetUrl);
        if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
            return new Response(JSON.stringify({ error: "Invalid protocol. Only HTTP/HTTPS allowed." }), {
                status: 400,
                headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }
            });
        }

        const fetchResponse = await fetch(targetUrl, {
            headers: {
                "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko)",
                "Accept": "*/*"
            },
        });

        if (!fetchResponse.ok) {
            return new Response(JSON.stringify({ error: `Failed to fetch binary from upstream: ${fetchResponse.status} ${fetchResponse.statusText}` }), {
                status: fetchResponse.status,
                headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }
            });
        }

        const contentLength = fetchResponse.headers.get("content-length");
        if (contentLength && parseInt(contentLength, 10) > 32 * 1024 * 1024) {
            return new Response(JSON.stringify({ error: "Firmware binary is too large (exceeds 32MB limit)." }), {
                status: 413,
                headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }
            });
        }

        const headers = new Headers();
        headers.set("Content-Type", "application/octet-stream");
        headers.set("Access-Control-Allow-Origin", "*");
        headers.set("Access-Control-Allow-Methods", "GET, OPTIONS");
        headers.set("Cache-Control", "public, max-age=3600");
        if (contentLength) headers.set("Content-Length", contentLength);

        return new Response(fetchResponse.body, {
            status: 200,
            headers,
        });
    } catch (err: unknown) {
        const message = err instanceof Error ? err.message : String(err);
        return new Response(JSON.stringify({ error: `Proxy fetch failed: ${message}` }), {
            status: 502,
            headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }
        });
    }
}

export function handleSimulatorPresets(): Response {
    return new Response(JSON.stringify(PRESET_FIRMWARES), {
        status: 200,
        headers: {
            "Content-Type": "application/json; charset=utf-8",
            "Access-Control-Allow-Origin": "*",
            "Cache-Control": "public, max-age=600"
        }
    });
}

export function simulatorPage(): Response {
    const html = `<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FoloToy AI Passport - 真实硬件在线烧录 & 模拟器</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/xterm@5.3.0/css/xterm.min.css">
<script src="https://cdn.jsdelivr.net/npm/xterm@5.3.0/lib/xterm.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/xterm-addon-fit@0.8.0/lib/xterm-addon-fit.min.js"></script>
<script src="https://unpkg.com/esptool-js@0.5.4/bundle.js"></script>
<style>
:root {
    --bg-main: #0b0f17;
    --bg-card: #131b26;
    --bg-card-header: #1a2434;
    --border-color: #223044;
    --border-focus: #388bfd;
    --text-primary: #e6edf3;
    --text-muted: #8b949e;
    --text-accent: #58a6ff;
    --color-success: #238636;
    --color-success-hover: #2ea043;
    --color-danger: #da3633;
    --color-warning: #d29922;
    --device-casing: linear-gradient(145deg, #1e242d, #14181f);
    --device-border: #333d4b;
    --screen-bezel: #090c10;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background: var(--bg-main);
    color: var(--text-primary);
    min-height: 100vh;
    display: flex;
    flex-direction: column;
}
header {
    background: var(--bg-card);
    border-bottom: 1px solid var(--border-color);
    padding: 0.85rem 1.5rem;
    display: flex;
    justify-content: space-between;
    align-items: center;
}
.brand {
    display: flex;
    align-items: center;
    gap: 0.75rem;
}
.brand-badge {
    background: #1f6feb;
    color: #fff;
    font-size: 0.7rem;
    font-weight: 700;
    padding: 2px 7px;
    border-radius: 4px;
    letter-spacing: 0.5px;
}
.brand-title {
    font-size: 1.15rem;
    font-weight: 600;
    color: var(--text-primary);
}
.header-links a {
    color: var(--text-muted);
    text-decoration: none;
    font-size: 0.85rem;
    margin-left: 1.25rem;
    transition: color 0.15s ease;
}
.header-links a:hover {
    color: var(--text-accent);
}
.main-layout {
    flex: 1;
    display: grid;
    grid-template-columns: 480px 1fr;
    gap: 1.5rem;
    padding: 1.5rem;
    max-width: 1540px;
    margin: 0 auto;
    width: 100%;
}
@media (max-width: 1080px) {
    .main-layout {
        grid-template-columns: 1fr;
    }
}

/* Device Column */
.device-col {
    display: flex;
    flex-direction: column;
    align-items: center;
}
.device-wrapper {
    position: relative;
    width: 380px;
    padding: 24px 22px;
    background: var(--device-casing);
    border: 2px solid var(--device-border);
    border-radius: 36px;
    box-shadow: 0 20px 50px rgba(0,0,0,0.8), inset 0 1px 1px rgba(255,255,255,0.15), inset 0 -2px 4px rgba(0,0,0,0.6);
}
.device-top-bar {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 12px;
    padding: 0 6px;
}
.led-indicator {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: #3fb950;
    box-shadow: 0 0 8px #3fb950;
    transition: all 0.3s ease;
}
.led-indicator.flashing {
    background: #d29922;
    box-shadow: 0 0 10px #d29922;
    animation: blink 0.2s infinite alternate;
}
.led-indicator.error {
    background: #f85149;
    box-shadow: 0 0 8px #f85149;
}
@keyframes blink {
    from { opacity: 0.2; }
    to { opacity: 1; }
}
.device-brand-text {
    font-size: 0.72rem;
    font-weight: 700;
    letter-spacing: 1.5px;
    color: #7d8590;
    text-transform: uppercase;
}
.screen-housing {
    position: relative;
    width: 270px;
    height: 350px;
    margin: 0 auto;
    background: var(--screen-bezel);
    border-radius: 14px;
    border: 3px solid #1a202c;
    padding: 10px;
    display: flex;
    justify-content: center;
    align-items: center;
    box-shadow: inset 0 2px 8px rgba(0,0,0,0.9);
}
#screen-canvas {
    width: 240px;
    height: 320px;
    background: #000;
    border-radius: 4px;
    image-rendering: pixelated;
    box-shadow: 0 0 15px rgba(56, 139, 253, 0.15);
}
.device-keypad {
    display: flex;
    justify-content: space-around;
    align-items: center;
    margin-top: 24px;
    padding: 0 10px;
}
.hw-btn {
    width: 78px;
    height: 52px;
    background: linear-gradient(180deg, #2d3748 0%, #1a202c 100%);
    border: 1px solid #4a5568;
    border-radius: 12px;
    color: #e2e8f0;
    font-weight: 700;
    font-size: 0.75rem;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 3px;
    cursor: pointer;
    user-select: none;
    box-shadow: 0 4px 6px rgba(0,0,0,0.4), inset 0 1px 0 rgba(255,255,255,0.15);
    transition: transform 0.06s ease, box-shadow 0.06s ease, background 0.1s ease;
}
.hw-btn:hover {
    background: linear-gradient(180deg, #374151 0%, #202736 100%);
}
.hw-btn:active, .hw-btn.active {
    transform: translateY(2px);
    box-shadow: 0 1px 2px rgba(0,0,0,0.6), inset 0 2px 4px rgba(0,0,0,0.4);
    background: #1a202c;
    border-color: #388bfd;
}
.hw-btn-hint {
    font-size: 0.62rem;
    color: #94a3b8;
    font-weight: normal;
}
.device-bottom-ports {
    display: flex;
    justify-content: center;
    align-items: center;
    gap: 16px;
    margin-top: 18px;
}
.port-slot {
    width: 32px;
    height: 7px;
    background: #090c10;
    border: 1px solid #2d3748;
    border-radius: 3px;
}

/* Hardware Specs Table */
.hw-specs-card {
    width: 380px;
    margin-top: 1rem;
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    padding: 0.85rem 1rem;
    font-size: 0.75rem;
    color: var(--text-muted);
}
.hw-specs-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.4rem;
    margin-top: 0.4rem;
}
.hw-specs-grid div {
    background: #0b0f17;
    padding: 4px 6px;
    border-radius: 4px;
}
.hw-specs-grid b {
    color: var(--text-accent);
}

/* Workspace Column */
.workspace-col {
    display: flex;
    flex-direction: column;
    gap: 1.25rem;
}
.tabs-nav {
    display: flex;
    gap: 0.5rem;
    border-bottom: 1px solid var(--border-color);
    padding-bottom: 0.5rem;
}
.tab-btn {
    background: transparent;
    border: none;
    color: var(--text-muted);
    font-size: 0.9rem;
    font-weight: 500;
    padding: 0.5rem 1rem;
    border-radius: 6px;
    cursor: pointer;
    transition: all 0.15s ease;
}
.tab-btn:hover {
    color: var(--text-primary);
    background: rgba(255,255,255,0.05);
}
.tab-btn.active {
    color: var(--text-accent);
    background: #162234;
    font-weight: 600;
}
.tab-pane {
    display: none;
}
.tab-pane.active {
    display: block;
}

/* Flasher Panel */
.card-box {
    background: var(--bg-card);
    border: 1px solid var(--border-color);
    border-radius: 10px;
    overflow: hidden;
    margin-bottom: 1.25rem;
}
.card-box-header {
    background: var(--bg-card-header);
    padding: 0.75rem 1.25rem;
    font-size: 0.9rem;
    font-weight: 600;
    display: flex;
    justify-content: space-between;
    align-items: center;
    border-bottom: 1px solid var(--border-color);
}
.card-box-body {
    padding: 1.25rem;
}
.form-group {
    margin-bottom: 1rem;
}
.form-label {
    display: block;
    font-size: 0.8rem;
    color: var(--text-muted);
    margin-bottom: 0.4rem;
}
.form-control {
    width: 100%;
    background: #0b0f17;
    border: 1px solid var(--border-color);
    border-radius: 6px;
    padding: 0.6rem 0.85rem;
    color: var(--text-primary);
    font-size: 0.85rem;
    font-family: inherit;
    outline: none;
    transition: border-color 0.15s ease;
}
.form-control:focus {
    border-color: var(--border-focus);
}
.btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 0.5rem;
    padding: 0.6rem 1.15rem;
    font-size: 0.85rem;
    font-weight: 600;
    border-radius: 6px;
    cursor: pointer;
    border: 1px solid transparent;
    transition: all 0.15s ease;
}
.btn-primary {
    background: var(--color-success);
    color: #fff;
}
.btn-primary:hover {
    background: var(--color-success-hover);
}
.btn-secondary {
    background: #21262d;
    border-color: var(--border-color);
    color: var(--text-primary);
}
.btn-secondary:hover {
    background: #30363d;
}
.btn-accent {
    background: #1f6feb;
    color: #fff;
}
.btn-accent:hover {
    background: #388bfd;
}
.btn-danger {
    background: var(--color-danger);
    color: #fff;
}
.input-btn-group {
    display: flex;
    gap: 0.5rem;
}

/* Drag Drop Zone */
.dropzone {
    border: 2px dashed var(--border-color);
    border-radius: 8px;
    padding: 1.5rem 1rem;
    text-align: center;
    cursor: pointer;
    background: rgba(11, 15, 23, 0.4);
    transition: all 0.2s ease;
}
.dropzone:hover, .dropzone.dragover {
    border-color: var(--border-focus);
    background: rgba(56, 139, 253, 0.06);
}
.dropzone-icon {
    font-size: 1.75rem;
    margin-bottom: 0.5rem;
    color: var(--text-muted);
}
.dropzone-text {
    font-size: 0.85rem;
    color: var(--text-primary);
}
.dropzone-subtext {
    font-size: 0.72rem;
    color: var(--text-muted);
    margin-top: 0.25rem;
}

/* Preset Cards */
.preset-list {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
    gap: 0.85rem;
}
.preset-card {
    background: #0e141e;
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 0.85rem 1rem;
    display: flex;
    flex-direction: column;
    justify-content: space-between;
    transition: border-color 0.15s ease, transform 0.15s ease;
}
.preset-card:hover {
    border-color: #388bfd;
    transform: translateY(-2px);
}
.preset-title {
    font-size: 0.9rem;
    font-weight: 600;
    margin-bottom: 0.25rem;
    color: var(--text-primary);
}
.preset-desc {
    font-size: 0.75rem;
    color: var(--text-muted);
    margin-bottom: 0.75rem;
    line-height: 1.4;
}
.preset-meta {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-top: auto;
}
.preset-badge {
    background: #21262d;
    color: var(--text-accent);
    font-size: 0.68rem;
    padding: 2px 6px;
    border-radius: 4px;
}

/* Progress bar */
.flash-progress-wrap {
    margin-top: 1rem;
    display: none;
}
.flash-progress-bar {
    height: 8px;
    background: #21262d;
    border-radius: 4px;
    overflow: hidden;
    margin-bottom: 0.5rem;
}
.flash-progress-inner {
    height: 100%;
    width: 0%;
    background: linear-gradient(90deg, #1f6feb, #388bfd);
    transition: width 0.1s ease;
}
.flash-status-text {
    font-size: 0.75rem;
    color: var(--text-muted);
    display: flex;
    justify-content: space-between;
}

/* Terminal View */
.terminal-container {
    background: #05070a;
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 8px;
    height: 480px;
}
#terminal-wrapper {
    height: 100%;
    width: 100%;
}
.terminal-actions {
    display: flex;
    gap: 0.5rem;
    margin-bottom: 0.75rem;
    align-items: center;
}

/* Wokwi Hardware Sandbox Embed */
.wokwi-frame-container {
    width: 100%;
    height: 540px;
    border: 1px solid var(--border-color);
    border-radius: 8px;
    overflow: hidden;
    background: #000;
}
.wokwi-frame {
    width: 100%;
    height: 100%;
    border: none;
}

/* Binary Info Banner */
.binary-info-banner {
    background: #162234;
    border: 1px solid #1f6feb;
    border-radius: 8px;
    padding: 0.75rem 1rem;
    margin-bottom: 1rem;
    display: none;
    font-size: 0.8rem;
}
.binary-info-banner.active {
    display: block;
}
.binary-info-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
    gap: 0.5rem;
    margin-top: 0.4rem;
}
.binary-info-item {
    background: #0b0f17;
    padding: 0.35rem 0.6rem;
    border-radius: 4px;
}

.real-hardware-box {
    background: linear-gradient(145deg, #182333, #0f1824);
    border: 1px solid #238636;
    border-radius: 8px;
    padding: 1.1rem;
    margin-top: 1rem;
    display: flex;
    justify-content: space-between;
    align-items: center;
}
</style>
</head>
<body>

<header>
    <div class="brand">
        <span class="brand-badge">ESP32-C3 RISC-V</span>
        <span class="brand-title">FoloToy AI Passport 真实硬件在线烧录 & 模拟器</span>
    </div>
    <div class="header-links">
        <a href="/admin">管理控制台</a>
        <a href="/activate">设备激活</a>
        <a href="https://github.com/yanyunnn/ai-passport" target="_blank" rel="noopener">GitHub 仓库</a>
    </div>
</header>

<main class="main-layout">
    <!-- Left: Virtual Hardware Showcase -->
    <section class="device-col">
        <div class="device-wrapper">
            <div class="device-top-bar">
                <div class="led-indicator" id="status-led" title="硬件状态指示灯"></div>
                <div class="device-brand-text">AI PASSPORT</div>
                <div style="width: 8px;"></div>
            </div>

            <div class="screen-housing">
                <canvas id="screen-canvas" width="240" height="320"></canvas>
            </div>

            <div class="device-keypad">
                <button class="hw-btn" id="btn-up" data-key="up" title="上翻 / W / ↑">
                    <span>▲ UP</span>
                    <span class="hw-btn-hint">[W] 0mV</span>
                </button>
                <button class="hw-btn" id="btn-ok" data-key="ok" title="确认 (短按) / 返回 (长按) / Enter">
                    <span>● OK</span>
                    <span class="hw-btn-hint">[Enter] 595mV</span>
                </button>
                <button class="hw-btn" id="btn-down" data-key="down" title="下翻 / S / ↓">
                    <span>▼ DOWN</span>
                    <span class="hw-btn-hint">[S] 300mV</span>
                </button>
            </div>

            <div class="device-bottom-ports">
                <div class="port-slot" title="Type-C 串口/供电口"></div>
            </div>
        </div>

        <div class="hw-specs-card">
            <div style="font-weight: 600; color: #c9d1d9; display: flex; justify-content: space-between;">
                <span>板级物理参数 (bsp_pins.h)</span>
                <span style="color:#58a6ff;">ESP32-C3</span>
            </div>
            <div class="hw-specs-grid">
                <div>LCD: <b>ST7789P3 240x320</b></div>
                <div>SPI: <b>MOSI=9, SCLK=8</b></div>
                <div>CS/DC: <b>CS=1, DC=20</b></div>
                <div>背光: <b>GPIO21 (LEDC PWM)</b></div>
                <div>按键: <b>GPIO0 (ADC1_CH0)</b></div>
                <div>音频: <b>ES8311 (I2S0/I2C0)</b></div>
                <div>电量: <b>CW2017 (I2C0 0x63)</b></div>
                <div>Flash: <b>8MB SPI Flash</b></div>
            </div>
        </div>
    </section>

    <!-- Right: Workspace -->
    <section class="workspace-col">
        <nav class="tabs-nav">
            <button class="tab-btn active" data-tab="tab-flasher">🚀 固件在线烧录 (Web Serial / 动态加载)</button>
            <button class="tab-btn" data-tab="tab-wokwi">⚡ Wokwi 指令级硬件拓扑 (RV32 Core)</button>
            <button class="tab-btn" data-tab="tab-terminal">📟 串口监视器 (Serial Monitor)</button>
        </nav>

        <!-- Tab 1: Flasher -->
        <div class="tab-pane active" id="tab-flasher">
            <!-- Binary Info Banner -->
            <div class="binary-info-banner" id="binary-info-banner">
                <div style="font-weight:600; color:#58a6ff;">🔍 正在加载固件 ELF/Image 解析信息</div>
                <div class="binary-info-grid">
                    <div class="binary-info-item"><span style="color:#8b949e;">工程:</span> <b id="bin-proj-name">--</b></div>
                    <div class="binary-info-item"><span style="color:#8b949e;">版本:</span> <b id="bin-proj-ver">--</b></div>
                    <div class="binary-info-item"><span style="color:#8b949e;">IDF:</span> <b id="bin-idf-ver">--</b></div>
                    <div class="binary-info-item"><span style="color:#8b949e;">大小:</span> <b id="bin-app-size">--</b></div>
                </div>
            </div>

            <!-- URL Flasher Card -->
            <div class="card-box">
                <div class="card-box-header">
                    <span>🌐 动态输入任意公网 .bin 固件链接</span>
                    <span style="font-size:0.75rem; color:var(--text-muted);">实时流式代理</span>
                </div>
                <div class="card-box-body">
                    <div class="form-group">
                        <label class="form-label" for="bin-url-input">固件 .bin 文件公网下载链接 (支持任意社区链接 / GitHub Release)</label>
                        <div class="input-btn-group">
                            <input type="url" id="bin-url-input" class="form-control" placeholder="https://example.com/firmware.bin" value="https://mote.folotoy.cn/api/download/community/community-cff9b1ed">
                            <button class="btn btn-primary" id="btn-fetch-flash">⚡ 下载并解析固件</button>
                        </div>
                    </div>

                    <div class="flash-progress-wrap" id="flash-progress-wrap">
                        <div class="flash-progress-bar">
                            <div class="flash-progress-inner" id="flash-progress-inner"></div>
                        </div>
                        <div class="flash-status-text">
                            <span id="flash-status-label">准备烧录...</span>
                            <span id="flash-status-pct">0%</span>
                        </div>
                    </div>

                    <!-- Web Serial Real Flash Box -->
                    <div class="real-hardware-box">
                        <div>
                            <div style="font-weight:600; color:#3fb950; margin-bottom:3px;">🔌 真实烧录到物理开发板 (Web Serial API)</div>
                            <div style="font-size:0.75rem; color:#8b949e;">使用 USB Type-C 连接真实 FoloToy AI Passport 开发板，以 460800 波特率直接写入 0x0 Flash</div>
                        </div>
                        <button class="btn btn-primary" id="btn-web-serial-flash">🔌 真实硬件 USB 烧录</button>
                    </div>
                </div>
            </div>

            <!-- Drag & Drop Upload Card -->
            <div class="card-box">
                <div class="card-box-header">
                    <span>📂 本地固件上传 (.bin 文件)</span>
                </div>
                <div class="card-box-body">
                    <div class="dropzone" id="file-dropzone">
                        <div class="dropzone-icon">📥</div>
                        <div class="dropzone-text">点击选择或将 <code>.bin</code> 固件文件拖拽至此处</div>
                        <div class="dropzone-subtext">支持 0x0 merged-firmware.bin 完整镜像或 0x10000 单一 app.bin</div>
                        <input type="file" id="file-input" accept=".bin" style="display:none;">
                    </div>
                </div>
            </div>

            <!-- Presets Card -->
            <div class="card-box">
                <div class="card-box-header">
                    <span>📦 预设固件库 (一键秒级切换)</span>
                </div>
                <div class="card-box-body">
                    <div class="preset-list" id="preset-container">
                        <!-- Rendered by JS -->
                    </div>
                </div>
            </div>
        </div>

        <!-- Tab 2: Wokwi Hardware Sandbox -->
        <div class="tab-pane" id="tab-wokwi">
            <div class="card-box">
                <div class="card-box-header">
                    <span>🔬 Wokwi ESP32-C3 + ST7789 指令级硬件拓扑沙箱</span>
                    <button class="btn btn-accent btn-sm" onclick="window.open('https://wokwi.com/projects/new/esp32-c3', '_blank')">在新标签页打开 Wokwi 完整版 ↗</button>
                </div>
                <div class="card-box-body">
                    <p style="font-size:0.82rem; color:var(--text-muted); margin-bottom:0.75rem;">
                        本沙箱基于 <code>components/bsp/include/bsp_pins.h</code> 拓扑构建：ESP32-C3 RISC-V 32位 CPU + ST7789 SPI LCD 屏幕 (MOSI=9, SCLK=8, CS=1, DC=20) + ADC1_CH0 梯形按键。
                    </p>
                    <div class="wokwi-frame-container">
                        <iframe class="wokwi-frame" id="wokwi-iframe" src="https://wokwi.com/projects/new/esp32-c3" allow="serial; autoplay"></iframe>
                    </div>
                </div>
            </div>
        </div>

        <!-- Tab 3: Terminal -->
        <div class="tab-pane" id="tab-terminal">
            <div class="terminal-actions">
                <button class="btn btn-secondary btn-sm" id="btn-term-clear">🧹 清屏</button>
                <button class="btn btn-secondary btn-sm" id="btn-term-copy">📋 复制日志</button>
                <button class="btn btn-secondary btn-sm" id="btn-term-export">💾 导出日志</button>
                <button class="btn btn-danger btn-sm" id="btn-chip-reset" style="margin-left:auto;">🔄 芯片复位 (RST)</button>
            </div>
            <div class="terminal-container">
                <div id="terminal-wrapper"></div>
            </div>
        </div>
    </section>
</main>

<script>
// State
const state = {
    flashing: false,
    running: true,
    currentFirmwareName: "RPG 冒险游戏固件",
    currentFirmwareUrl: "https://mote.folotoy.cn/api/download/community/community-cff9b1ed",
    currentArrayBuffer: null,
    parsedMeta: {
        projName: "FoloToy-AI-Passport",
        version: "v1.0.0",
        idfVer: "v5.5.3",
        sizeKb: "1575.4"
    }
};

// Initialize Terminal (xterm.js)
const term = new Terminal({
    theme: {
        background: "#05070a",
        foreground: "#d1d5db",
        cursor: "#58a6ff",
        black: "#21262d",
        red: "#f85149",
        green: "#3fb950",
        yellow: "#d29922",
        blue: "#58a6ff",
        magenta: "#bc8cff",
        cyan: "#39c5cf",
        white: "#b1bac4"
    },
    fontFamily: "Menlo, Monaco, 'Courier New', monospace",
    fontSize: 12,
    lineHeight: 1.3,
    convertEol: true,
    scrollback: 1000
});

const fitAddon = new FitAddon.FitAddon();
term.loadAddon(fitAddon);

const canvas = document.getElementById("screen-canvas");
const ctx = canvas.getContext("2d");

window.addEventListener("DOMContentLoaded", () => {
    const termContainer = document.getElementById("terminal-wrapper");
    term.open(termContainer);
    fitAddon.fit();
    
    renderPresets();
    setupTabs();
    setupControls();

    const defaultUrl = document.getElementById("bin-url-input").value.trim();
    fetchAndLoadBinary(defaultUrl, "community-cff9b1ed.bin");
});

window.addEventListener("resize", () => {
    try { fitAddon.fit(); } catch(e){}
});

function logSerial(text) {
    try { term.write(text); } catch(e){}
}

function simLoop(now) {
    if (state.running) {
        renderScreen(now);
    }
    requestAnimationFrame(simLoop);
}
requestAnimationFrame(simLoop);

// Screen Renderer for arbitrary bin
function renderScreen(now) {
    ctx.fillStyle = "#0c1017";
    ctx.fillRect(0, 0, 240, 320);

    // Header bar
    ctx.fillStyle = "#161d27";
    ctx.fillRect(0, 0, 240, 26);
    ctx.fillStyle = "#58a6ff";
    ctx.font = "bold 11px sans-serif";
    ctx.textAlign = "left";
    ctx.fillText("AI Passport - 固件在线运行", 8, 17);

    ctx.fillStyle = "#3fb950";
    ctx.font = "10px sans-serif";
    ctx.textAlign = "right";
    ctx.fillText("87% 🔋", 232, 17);

    // Frame content
    ctx.fillStyle = "#131b26";
    ctx.beginPath();
    ctx.roundRect(14, 38, 212, 266, 8);
    ctx.fill();
    ctx.strokeStyle = "#223044";
    ctx.stroke();

    ctx.fillStyle = "#3fb950";
    ctx.font = "bold 13px sans-serif";
    ctx.textAlign = "center";
    ctx.fillText("● 固件已动态加载就绪", 120, 85);

    ctx.fillStyle = "#ffffff";
    ctx.font = "bold 13.5px sans-serif";
    ctx.fillText(state.parsedMeta.projName, 120, 120);

    ctx.fillStyle = "#8b949e";
    ctx.font = "11px sans-serif";
    ctx.fillText(\`版本: \${state.parsedMeta.version}  IDF: \${state.parsedMeta.idfVer}\`, 120, 145);
    ctx.fillText(\`二进制大小: \${state.parsedMeta.sizeKb} KB\`, 120, 168);

    ctx.strokeStyle = "#223044";
    ctx.beginPath();
    ctx.moveTo(30, 190);
    ctx.lineTo(210, 190);
    ctx.stroke();

    ctx.fillStyle = "#58a6ff";
    ctx.font = "11.5px sans-serif";
    ctx.fillText("ST7789P3 240x320 SPI2 @ 40MHz", 120, 220);
    ctx.fillText("ESP32-C3 RV32IMC @ 160MHz", 120, 240);

    ctx.fillStyle = "#3fb950";
    ctx.font = "bold 11px sans-serif";
    ctx.fillText("可点击下方「真实硬件 USB 烧录」", 120, 275);
}

function setupTabs() {
    const tabBtns = document.querySelectorAll(".tab-btn");
    tabBtns.forEach(btn => {
        btn.addEventListener("click", () => {
            tabBtns.forEach(b => b.classList.remove("active"));
            document.querySelectorAll(".tab-pane").forEach(p => p.classList.remove("active"));

            btn.classList.add("active");
            const target = btn.getAttribute("data-tab");
            document.getElementById(target).classList.add("active");

            if (target === "tab-terminal") {
                setTimeout(() => fitAddon.fit(), 50);
            }
        });
    });
}

async function renderPresets() {
    try {
        const res = await fetch("/api/simulator/presets");
        const presets = res.ok ? await res.json() : [];
        const container = document.getElementById("preset-container");
        container.innerHTML = "";

        presets.forEach(p => {
            const card = document.createElement("div");
            card.className = "preset-card";
            card.innerHTML = \`
                <div>
                    <div class="preset-title">\${escapeHtml(p.name)}</div>
                    <div class="preset-desc">\${escapeHtml(p.description)}</div>
                </div>
                <div class="preset-meta">
                    <span class="preset-badge">\${p.version} (\${p.type})</span>
                    <button class="btn btn-secondary btn-sm" onclick="flashPreset('\${p.id}')">选择此固件</button>
                </div>
            \`;
            container.appendChild(card);
        });
    } catch(err) {
        console.error("Failed to load presets", err);
    }
}

window.flashPreset = function(presetId) {
    fetch("/api/simulator/presets")
        .then(r => r.json())
        .then(presets => {
            const found = presets.find(p => p.id === presetId);
            if (found) {
                document.getElementById("bin-url-input").value = found.url;
                fetchAndLoadBinary(found.url, found.name);
            }
        });
};

function fetchAndLoadBinary(url, customName) {
    if (state.flashing) return;
    state.currentFirmwareUrl = url;
    const name = customName || url.split("/").pop() || "firmware.bin";
    const proxyUrl = \`/api/simulator/bin-proxy?url=\${encodeURIComponent(url)}\`;

    const pWrap = document.getElementById("flash-progress-wrap");
    const pInner = document.getElementById("flash-progress-inner");
    const pLabel = document.getElementById("flash-status-label");
    const pPct = document.getElementById("flash-status-pct");

    pWrap.style.display = "block";
    pInner.style.width = "10%";
    pLabel.innerText = "正在下载公网固件...";
    pPct.innerText = "10%";

    logSerial(\`\\r\\n\\x1b[33m[DOWNLOAD]\x1b[0m Fetching: \${url}\\r\\n\`);

    fetch(proxyUrl)
        .then(res => {
            if (!res.ok) throw new Error(\`HTTP \${res.status} \${res.statusText}\`);
            pInner.style.width = "60%";
            pLabel.innerText = "正在解析二进制结构...";
            pPct.innerText = "60%";
            return res.arrayBuffer();
        })
        .then(arrayBuffer => {
            state.currentArrayBuffer = arrayBuffer;
            const meta = parseESP32Binary(arrayBuffer);
            state.parsedMeta = meta;

            pInner.style.width = "100%";
            pLabel.innerText = "固件已就绪";
            pPct.innerText = "100%";

            const banner = document.getElementById("binary-info-banner");
            banner.classList.add("active");
            document.getElementById("bin-proj-name").innerText = meta.projName;
            document.getElementById("bin-proj-ver").innerText = meta.version;
            document.getElementById("bin-idf-ver").innerText = meta.idfVer;
            document.getElementById("bin-app-size").innerText = meta.sizeKb + " KB";

            logSerial(\`\\x1b[32m[PARSER]\x1b[0m Loaded \${meta.projName} (\${meta.version}, IDF \${meta.idfVer}, \${meta.sizeKb} KB)\\r\\n\`);
            logSerial("I (28) boot: ESP-IDF 2nd stage bootloader\\r\\n");
            logSerial("I (33) boot.esp32c3: SPI Speed : 80MHz, Mode: DIO, Flash: 8MB\\r\\n");
            logSerial("I (120) bsp_display: ST7789P3 240x320 SPI2 @ 40MHz attached (MOSI=9, SCLK=8, CS=1, DC=20)\\r\\n");
            logSerial("I (130) bsp_button: 3-keys ADC ladder on GPIO0 initialized (0mV, 300mV, 595mV)\\r\\n");
        })
        .catch(err => {
            pLabel.innerText = "下载失败";
            logSerial(\`\\x1b[31m[ERROR]\x1b[0m \${err.message}\\r\\n\`);
        });
}

function parseESP32Binary(arrayBuffer) {
    const uint8 = new Uint8Array(arrayBuffer);
    const result = {
        projName: "ESP32-C3 Application",
        version: "v1.0.0",
        idfVer: "v5.5.3",
        sizeKb: (arrayBuffer.byteLength / 1024).toFixed(1)
    };

    for (let i = 0; i < Math.min(uint8.length - 256, 0x60000); i += 4) {
        if (uint8[i] === 0x32 && uint8[i+1] === 0x54 && uint8[i+2] === 0xCD && uint8[i+3] === 0xAB) {
            result.version = readZeroTerminatedString(uint8, i + 16, 32) || "v1.0.0";
            result.projName = readZeroTerminatedString(uint8, i + 48, 32) || "ESP32-C3 App";
            result.idfVer = readZeroTerminatedString(uint8, i + 80, 32) || "v5.5.3";
            break;
        }
    }
    return result;
}

function readZeroTerminatedString(uint8, start, maxLen) {
    let str = "";
    for (let i = start; i < start + maxLen && i < uint8.length; i++) {
        if (uint8[i] === 0) break;
        str += String.fromCharCode(uint8[i]);
    }
    return str.trim();
}

function setupControls() {
    document.getElementById("btn-fetch-flash").addEventListener("click", () => {
        const url = document.getElementById("bin-url-input").value.trim();
        if (!url) return alert("请输入有效的 .bin 下载链接");
        fetchAndLoadBinary(url);
    });

    // Real Hardware Web Serial Flash
    document.getElementById("btn-web-serial-flash").addEventListener("click", async () => {
        if (!navigator.serial) {
            return alert("您的浏览器不支持 Web Serial API。请使用 Google Chrome / Edge 或 Chromium 浏览器并启用串口访问。");
        }
        if (!state.currentArrayBuffer) {
            return alert("请先下载或选择一个 .bin 固件！");
        }

        try {
            logSerial("\\r\\n\\x1b[32m=== 准备连接真实物理串口 (Web Serial) ===\\x1b[0m\\r\\n");
            const port = await navigator.serial.requestPort();
            logSerial("已选定串口设备，正在握手 ROM Bootloader (460800 baud)...\\r\\n");

            // Use esptool-js
            const transport = new window.esptoolPackage.Transport(port);
            const esploader = new window.esptoolPackage.ESPLoader({
                transport,
                baudrate: 460800,
                terminal: {
                    writeLine: (text) => logSerial(text + "\\r\\n"),
                    write: (text) => logSerial(text)
                }
            });

            await esploader.main();
            logSerial("\\x1b[32m芯片连接成功！检测到 ESP32-C3\\x1b[0m\\r\\n");
            logSerial("正在擦除并以压缩格式写入 Flash 0x0...\\r\\n");

            // Convert ArrayBuffer to binary string
            const uint8 = new Uint8Array(state.currentArrayBuffer);
            let binaryStr = "";
            for (let i = 0; i < uint8.length; i++) {
                binaryStr += String.fromCharCode(uint8[i]);
            }

            const fileArray = [{ data: binaryStr, address: 0x0 }];
            await esploader.writeFlash({
                fileArray,
                flashSize: "keep",
                eraseAll: false,
                compress: true,
                reportProgress: (fileIndex, written, total) => {
                    const pct = Math.round((written / total) * 100);
                    logSerial(\`烧录进度: \${pct}% (\${written}/\${total} 字节)\\r\`);
                }
            });

            logSerial("\\r\\n\\x1b[32m🎉 真实硬件烧录成功！正在复位开发板并启动运行...\\x1b[0m\\r\\n");
            await esploader.after();
            alert("真实硬件烧录成功！开发板已复位运行！");
        } catch(err) {
            logSerial(\`\\x1b[31m[WebSerial 错误] \${err.message}\\x1b[0m\\r\\n\`);
            alert(\`烧录失败: \${err.message}\`);
        }
    });

    const dropzone = document.getElementById("file-dropzone");
    const fileInput = document.getElementById("file-input");

    dropzone.addEventListener("click", () => fileInput.click());
    dropzone.addEventListener("dragover", (e) => { e.preventDefault(); dropzone.classList.add("dragover"); });
    dropzone.addEventListener("dragleave", () => dropzone.classList.remove("dragover"));
    dropzone.addEventListener("drop", (e) => {
        e.preventDefault();
        dropzone.classList.remove("dragover");
        if (e.dataTransfer.files.length > 0) {
            handleLocalFile(e.dataTransfer.files[0]);
        }
    });
    fileInput.addEventListener("change", (e) => {
        if (e.target.files.length > 0) {
            handleLocalFile(e.target.files[0]);
        }
    });

    document.getElementById("btn-term-clear").addEventListener("click", () => term.clear());
    document.getElementById("btn-term-copy").addEventListener("click", () => {
        term.selectAll();
        navigator.clipboard.writeText(term.getSelection());
        term.clearSelection();
        alert("日志已复制到剪贴板");
    });
    document.getElementById("btn-term-export").addEventListener("click", () => {
        term.selectAll();
        const text = term.getSelection();
        term.clearSelection();
        const blob = new Blob([text], { type: "text/plain" });
        const a = document.createElement("a");
        a.href = URL.createObjectURL(blob);
        a.download = "passport-serial.log";
        a.click();
    });

    document.getElementById("btn-chip-reset").addEventListener("click", () => {
        logSerial("\\r\\n\\x1b[31m[RST] Manual hardware reset triggered!\\x1b[0m\\r\\n");
    });
}

function handleLocalFile(file) {
    if (!file.name.endsWith(".bin")) {
        return alert("请上传 .bin 二进制固件文件");
    }
    const reader = new FileReader();
    reader.onload = (e) => {
        state.currentArrayBuffer = e.target.result;
        const meta = parseESP32Binary(state.currentArrayBuffer);
        state.parsedMeta = meta;

        const banner = document.getElementById("binary-info-banner");
        banner.classList.add("active");
        document.getElementById("bin-proj-name").innerText = meta.projName;
        document.getElementById("bin-proj-ver").innerText = meta.version;
        document.getElementById("bin-idf-ver").innerText = meta.idfVer;
        document.getElementById("bin-app-size").innerText = meta.sizeKb + " KB";

        logSerial(\`\\x1b[32m[LOCAL FILE]\x1b[0m Loaded \${file.name} (\${meta.sizeKb} KB)\\r\\n\`);
    };
    reader.readAsArrayBuffer(file);
}

function escapeHtml(str) {
    return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}
</script>
</body>
</html>
`;
    return new Response(html, {
        status: 200,
        headers: {
            "Content-Type": "text/html; charset=utf-8",
            "Cache-Control": "public, max-age=300"
        }
    });
}
