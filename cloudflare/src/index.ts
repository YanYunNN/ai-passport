import {
    bearerToken,
    hashDeviceCredential,
    hashEnrollmentCode,
    hasBearerSecret,
    issueDeviceCredential,
    issueUserCode,
    normalizeUserCode,
    verifyAdminBasicAuth,
    verifyDeviceCredential,
} from "./auth";
import { createRequestIndex, getRequestIndex, listDeviceEvents, listHookNotifyLogs, writeHookNotifyLog, type DeviceEventRow, type HookNotifyLogRow } from "./db";
import type { Env } from "./env";
import { PassportRelay } from "./passport-relay";
import { isDeviceId, parseApprovalInput, REQUEST_ID_PATTERN, type ApprovalInput } from "./protocol";
import { handleSimulatorBinProxy, handleSimulatorPresets, simulatorPage } from "./simulator";
import { renderWallpaperJpeg, weatherCodeLabel } from "./wallpaper";

export { PassportRelay };

const enrollmentLifetimeSeconds = 10 * 60;
const enrollmentPollIntervalSeconds = 5;

type EnrollmentStatus = "pending" | "approved" | "denied" | "consumed" | "expired";

interface EnrollmentRecord {
    enrollment_id: string;
    device_id: string;
    device_code_hash: string;
    user_code_hash: string;
    status: EnrollmentStatus;
    expires_at: number;
    poll_interval_seconds: number;
    last_polled_at: number | null;
}

interface DeviceRow {
    device_id: string;
    status: "active" | "revoked";
    credential_version: number;
    created_at: number;
    rotated_at: number | null;
}

interface PendingEnrollmentRow {
    enrollment_id: string;
    device_id: string;
    status: string;
    expires_at: number;
    created_at: number;
}

interface DeviceImageRow {
    image_id: string;
    device_id: string;
    title: string | null;
    image_data: string;
    created_at: number;
}

interface ApprovalLogRow {
    request_id: string;
    device_id: string;
    tool: string;
    summary: string;
    status: string;
    reason: string | null;
    created_at: number;
    decided_at: number | null;
    expires_at: number;
}

const json = (body: unknown, status = 200, additionalHeaders?: HeadersInit): Response => {
    const headers = new Headers({ "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" });
    if (additionalHeaders) {
        for (const [key, value] of new Headers(additionalHeaders)) headers.set(key, value);
    }
    return new Response(JSON.stringify(body), { status, headers });
};

const nowSeconds = (): number => Math.floor(Date.now() / 1000);

export default {
    async fetch(request: Request, env: Env): Promise<Response> {
        try {
            const url = new URL(request.url);
            if (request.method === "GET" && url.pathname === "/healthz") return json({ ok: true });
            if (request.method === "GET" && url.pathname === "/simulator") return simulatorPage();
            if (request.method === "GET" && url.pathname === "/api/simulator/bin-proxy") {
                return handleSimulatorBinProxy(request);
            }
            if (request.method === "GET" && url.pathname === "/api/simulator/presets") {
                return handleSimulatorPresets();
            }

            if (request.method === "GET" && (url.pathname === "/admin" || url.pathname === "/admin/devices")) {
                return adminDashboardPage(request, env);
            }
            if (request.method === "POST" && url.pathname === "/admin/devices/revoke") {
                return adminRevokeDeviceWeb(request, env);
            }
            if (request.method === "POST" && url.pathname === "/admin/devices/rotate") {
                return adminRotateDeviceWeb(request, env);
            }
            if (request.method === "POST" && url.pathname === "/admin/devices/delete") {
                return adminDeleteDeviceWeb(request, env);
            }
            if (request.method === "POST" && url.pathname === "/admin/devices/images/push") {
                return adminPushImageWeb(request, env);
            }
            if (request.method === "POST" && url.pathname === "/admin/devices/images/delete") {
                return adminDeleteImageWeb(request, env);
            }
            if (request.method === "GET" && url.pathname === "/admin/screencast/ws") {
                return adminScreencastWs(request, env);
            }
            if (request.method === "POST" && url.pathname === "/admin/screencast/command") {
                return adminScreencastCommand(request, env);
            }
            if (request.method === "GET" && url.pathname === "/admin/screencast/latest") {
                return adminScreencastLatest(request, env);
            }
            if (request.method === "POST" && url.pathname === "/admin/push") {
                return adminPushWeb(request, env);
            }
            if (request.method === "GET" && url.pathname === "/admin/push/status") {
                return adminPushStatusWeb(request, env);
            }
            if (request.method === "POST" && url.pathname === "/admin/hook/push") {
                return adminHookPush(request, env);
            }
            if (request.method === "GET" && url.pathname === "/admin/dashboard-data") {
                return adminDashboardData(request, env);
            }
            if (request.method === "GET" && url.pathname === "/admin/monitoring") {
                return adminMonitoringWeb(request, env);
            }
            if (request.method === "GET" && url.pathname === "/admin/wallpaper/preview") {
                return adminWallpaperPreviewWeb(request, env);
            }
            if (request.method === "POST" && url.pathname === "/admin/wallpaper/push") {
                return adminWallpaperPushWeb(request, env);
            }
            if (request.method === "POST" && url.pathname === "/admin/wallpaper/notes") {
                return adminWallpaperNotesWeb(request, env);
            }

            if (request.method === "GET" && url.pathname === "/activate") return activationPage();
            if (request.method === "POST" && url.pathname === "/activate") return approveEnrollment(request, env);
            if (request.method === "GET" && url.pathname === "/admin/pair") return adminPairPage(request, env);
            if (request.method === "POST" && url.pathname === "/admin/pair") return adminPair(request, env);
            if (request.method === "POST" && url.pathname === "/v1/enrollment/device-code") {
                return createDeviceCode(request, env);
            }
            if (request.method === "POST" && url.pathname === "/v1/enrollment/token") return exchangeDeviceCode(request, env);
            if (request.method === "POST" && url.pathname === "/v1/enrollment/approve") return approveEnrollment(request, env);

            const deviceMatch = url.pathname.match(/^\/device\/(passport-[A-F0-9]{12})$/u);
            if (request.method === "GET" && deviceMatch) return connectDevice(request, env, deviceMatch[1]);

            if (request.method === "GET" && url.pathname === "/v1/admin/devices") {
                return listDevicesApi(request, env);
            }
            if (request.method === "POST" && url.pathname === "/v1/admin/devices") {
                return registerDevice(request, env);
            }
            const adminDeviceMatch = url.pathname.match(/^\/v1\/admin\/devices\/(passport-[A-F0-9]{12})$/u);
            if (request.method === "DELETE" && adminDeviceMatch) return revokeDevice(request, env, adminDeviceMatch[1]);
            const rotateMatch = url.pathname.match(/^\/v1\/admin\/devices\/(passport-[A-F0-9]{12})\/rotate$/u);
            if (request.method === "POST" && rotateMatch) return rotateDevice(request, env, rotateMatch[1]);

            const createMatch = url.pathname.match(/^\/v1\/devices\/(passport-[A-F0-9]{12})\/requests$/u);
            if (request.method === "POST" && createMatch) return createApproval(request, env, createMatch[1]);
            const notifyMatch = url.pathname.match(/^\/v1\/devices\/(passport-[A-F0-9]{12})\/notify$/u);
            if (request.method === "POST" && notifyMatch) return pushNotify(request, env, notifyMatch[1]);
            const statusMatch = url.pathname.match(/^\/v1\/requests\/([0-9a-f-]{1,36})$/u);
            if (request.method === "GET" && statusMatch) return getApproval(request, env, statusMatch[1]);
            return json({ error: "not found" }, 404);
        } catch (error) {
            console.error("Unhandled relay error", error);
            return json({ error: "relay unavailable" }, 503);
        }
    },
    async scheduled(event: ScheduledEvent, env: Env): Promise<void> {
        try {
            await generateAndPushWallpaper(env);
        } catch (err) {
            console.error("Wallpaper scheduled push failed", err);
        }
    },
};

async function checkDeviceOnline(env: Env, deviceId: string): Promise<boolean> {
    try {
        const id = env.PASSPORTS.idFromName(deviceId);
        const res = await env.PASSPORTS.get(id).fetch("https://passport.internal/internal/status");
        if (res.ok) {
            const data = await res.json<{ online?: boolean }>();
            return Boolean(data?.online);
        }
    } catch {}
    return false;
}

/* 后台所有时间统一以 +08:00（Asia/Shanghai）时区展示，输出 YYYY-MM-DD HH:mm:ss +08:00。 */
function formatDate(timestamp: number | null): string {
    if (!timestamp) return "-";
    const shifted = new Date((timestamp + 8 * 3600) * 1000);
    const pad = (n: number): string => String(n).padStart(2, "0");
    return `${shifted.getUTCFullYear()}-${pad(shifted.getUTCMonth() + 1)}-${pad(shifted.getUTCDate())} ` +
        `${pad(shifted.getUTCHours())}:${pad(shifted.getUTCMinutes())}:${pad(shifted.getUTCSeconds())} +08:00`;
}

function activationPage(): Response {
    return htmlPage("Pair Passport device", `
        <div class="nav-bar">
            <a href="/admin" class="nav-link">← 返回管理控制台</a>
        </div>
        <p class="desc">Enter the 6-digit code shown by your device. This legacy page approves immediately after submission.</p>
        <form method="post" action="/activate" class="pair-card">
            <label for="user_code" class="form-label">设备 6 位配对码</label>
            <input id="user_code" name="user_code" inputmode="numeric" pattern="[0-9]{6}" autocomplete="one-time-code" spellcheck="false" maxlength="6" placeholder="000000" class="code-input" required autofocus>
            <button type="submit" class="btn btn-primary btn-block">Approve device</button>
        </form>
    `);
}

function pairingPage(title: string, message: string, action: string, button: string, nav: string): Response {
    return htmlPage(title, `
        <p class="desc">${message}</p>
        <form method="post" action="${action}" class="pair-card">
            <label for="user_code" class="form-label">设备 6 位配对码</label>
            <input id="user_code" name="user_code" inputmode="numeric" pattern="[0-9]{6}" autocomplete="one-time-code" spellcheck="false" maxlength="6" placeholder="000000" class="code-input" required autofocus>
            <button type="submit" class="btn btn-primary btn-block">${button}</button>
        </form>
    `, 200, undefined, nav);
}

function renderNav(active: string | null): string {
    const tabs: Array<{ id: string; label: string }> = [
        { id: "overview", label: "📊 概览" },
        { id: "snapshot", label: "📸 远程快照" },
        { id: "images", label: "🖼️ 图片推送" },
        { id: "wallpaper", label: "📟 信息壁纸" },
        { id: "requests", label: "📨 审批推送" },
        { id: "notify", label: "🔔 通知推送" },
        { id: "devices", label: "📱 设备管理" },
    ];
    const tabHtml = tabs
        .map((tab) => `<button type="button" class="nav-tab${active === tab.id ? " active" : ""}" data-tab="${tab.id}">${tab.label}</button>`)
        .join("");
    return `
    <nav class="topnav">
        <div class="topnav-inner">
            <a href="/admin" class="brand">🛂 <span>Kiro Passport</span></a>
            <div class="nav-tabs">${tabHtml}</div>
            <div class="topnav-actions">
                <a href="/simulator" class="btn btn-sm btn-accent" target="_blank">🎮 模拟器</a>
                <a href="/admin" class="btn btn-sm nav-btn">🔄 刷新</a>
                <a href="/admin/pair" class="btn btn-sm btn-primary">➕ 配对新设备</a>
            </div>
        </div>
    </nav>`;
}

function renderSimpleNav(): string {
    return `
    <nav class="topnav">
        <div class="topnav-inner">
            <a href="/admin" class="brand">🛂 <span>Kiro Passport</span></a>
            <div class="topnav-actions">
                <a href="/admin" class="btn btn-sm nav-btn">← 返回管理控制台</a>
            </div>
        </div>
    </nav>`;
}

function htmlPage(title: string, content: string, status = 200, additionalHeaders?: HeadersInit, nav = ""): Response {
    const body = `<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escapeHtml(title)} - Kiro Passport Relay</title>
<style>
:root {
    --bg: #0d1117;
    --card-bg: #161b22;
    --border: #30363d;
    --text: #e6edf3;
    --text-muted: #8b949e;
    --primary: #238636;
    --primary-hover: #2ea043;
    --accent: #1f6feb;
    --accent-hover: #388bfd;
    --accent-soft: rgba(31, 111, 235, 0.14);
    --danger: #da3633;
    --danger-hover: #f85149;
    --online: #3fb950;
    --offline: #6e7681;
    --radius: 10px;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background: var(--bg);
    background-image:
        radial-gradient(1200px 500px at 20% -10%, rgba(31, 111, 235, 0.08), transparent 60%),
        radial-gradient(900px 420px at 90% 0%, rgba(35, 134, 54, 0.06), transparent 60%);
    color: var(--text);
    line-height: 1.5;
    min-height: 100vh;
}
.container { max-width: 960px; margin: 0 auto; padding: 2rem 1rem 0.5rem; }

/* Top navigation */
.topnav {
    position: sticky;
    top: 0;
    z-index: 100;
    background: rgba(13, 17, 23, 0.85);
    backdrop-filter: blur(14px);
    -webkit-backdrop-filter: blur(14px);
    border-bottom: 1px solid var(--border);
}
.topnav-inner {
    max-width: 960px;
    margin: 0 auto;
    padding: 0.6rem 1rem;
    display: flex;
    align-items: center;
    gap: 1rem;
    flex-wrap: wrap;
}
.brand {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
    color: var(--text);
    text-decoration: none;
    font-size: 1.05rem;
    font-weight: 700;
    letter-spacing: 0.01em;
}
.brand span {
    background: linear-gradient(90deg, #58a6ff, #3fb950);
    -webkit-background-clip: text;
    background-clip: text;
    -webkit-text-fill-color: transparent;
}
.nav-tabs { display: flex; gap: 0.3rem; flex-wrap: wrap; }
.nav-tab {
    background: transparent;
    border: 1px solid transparent;
    color: var(--text-muted);
    padding: 0.4rem 0.85rem;
    border-radius: 8px;
    font-size: 0.88rem;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.15s ease;
}
.nav-tab:hover { background: rgba(255,255,255,0.06); color: var(--text); }
.nav-tab.active { background: var(--accent-soft); border-color: rgba(31,111,235,0.35); color: #58a6ff; }
.topnav-actions { margin-left: auto; display: flex; gap: 0.5rem; align-items: center; }
.nav-btn { background: #21262d; border: 1px solid var(--border); color: var(--text); }
.nav-btn:hover { border-color: #484f58; }

/* Page sections (tabbed) */
.page { display: none; }
.page.active { display: block; animation: pageIn 0.28s ease; }
@keyframes pageIn {
    from { opacity: 0; transform: translateY(8px); }
    to { opacity: 1; transform: none; }
}
.page-head { margin-bottom: 1.5rem; }
.page-head h1 { font-size: 1.35rem; font-weight: 700; margin-bottom: 0.3rem; }
.page-desc { color: var(--text-muted); font-size: 0.9rem; }
.section-title { font-size: 0.9rem; font-weight: 600; color: var(--text-muted); margin: 0 0 0.9rem; }

/* Legacy nav on public pairing pages */
.nav-bar { margin-bottom: 1.5rem; }
.nav-link { color: var(--accent); text-decoration: none; font-size: 0.9rem; }
.nav-link:hover { text-decoration: underline; }

/* Stats */
.stats-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 1rem;
    margin-bottom: 2rem;
}
.stat-card {
    background: linear-gradient(180deg, #1b222c 0%, var(--card-bg) 100%);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 1.3rem 1.4rem;
    position: relative;
    overflow: hidden;
    transition: border-color 0.2s ease, transform 0.2s ease, box-shadow 0.2s ease;
}
.stat-card::after {
    content: "";
    position: absolute;
    top: 0; left: 0; right: 0;
    height: 2px;
    background: linear-gradient(90deg, var(--accent), transparent 70%);
    opacity: 0.6;
}
.stat-card:hover { border-color: #3d4650; transform: translateY(-2px); box-shadow: 0 10px 28px rgba(0,0,0,0.35); }
.stat-icon { font-size: 1.5rem; margin-bottom: 0.6rem; }
.stat-label { color: var(--text-muted); font-size: 0.85rem; margin-bottom: 0.25rem; }
.stat-value { font-size: 1.75rem; font-weight: 700; }

/* Quick actions */
.quick-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 1rem; }
.quick-card {
    display: flex;
    align-items: center;
    gap: 0.9rem;
    background: var(--card-bg);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 1.15rem 1.25rem;
    text-decoration: none;
    color: var(--text);
    transition: border-color 0.2s ease, transform 0.2s ease, box-shadow 0.2s ease;
}
.quick-card:hover { border-color: rgba(31,111,235,0.6); transform: translateY(-2px); box-shadow: 0 10px 28px rgba(0,0,0,0.35); }
.quick-icon { font-size: 1.7rem; }
.quick-title { font-weight: 600; font-size: 0.95rem; }
.quick-desc { color: var(--text-muted); font-size: 0.78rem; margin-top: 0.15rem; }

.card {
    background: var(--card-bg);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    overflow: hidden;
    margin-bottom: 2rem;
    transition: border-color 0.2s ease;
}
.card:hover { border-color: #3d4650; }
.card-header {
    padding: 1rem 1.25rem;
    border-bottom: 1px solid var(--border);
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 1rem;
    flex-wrap: wrap;
}
.card-title { font-size: 1.05rem; font-weight: 600; }
table { width: 100%; border-collapse: collapse; text-align: left; font-size: 0.9rem; }
th, td { padding: 0.85rem 1.25rem; border-bottom: 1px solid var(--border); }
th { background: #13171e; color: var(--text-muted); font-weight: 600; }
tr:last-child td { border-bottom: none; }
tr:hover td { background: rgba(255,255,255,0.02); }
.badge {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    padding: 0.2rem 0.55rem;
    border-radius: 20px;
    font-size: 0.75rem;
    font-weight: 600;
}
.badge-active { background: rgba(46, 160, 67, 0.15); color: #3fb950; border: 1px solid rgba(46, 160, 67, 0.3); }
.badge-revoked { background: rgba(248, 81, 73, 0.15); color: #f85149; border: 1px solid rgba(248, 81, 73, 0.3); }
.dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; }
.dot-online { background: var(--online); box-shadow: 0 0 6px var(--online); }
.dot-offline { background: var(--offline); }
.code-mono { font-family: ui-monospace, SFMono-Regular, Consolas, monospace; font-size: 0.9em; background: #21262d; padding: 0.15rem 0.4rem; border-radius: 4px; }
.btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 0.45rem 0.9rem;
    border-radius: 6px;
    font-size: 0.85rem;
    font-weight: 500;
    cursor: pointer;
    border: 1px solid transparent;
    text-decoration: none;
    transition: all 0.15s ease;
}
.btn-primary { background: var(--primary); color: #fff; border-color: rgba(255,255,255,0.1); }
.btn-primary:hover { background: var(--primary-hover); }
.btn-accent { background: var(--accent); color: #fff; }
.btn-accent:hover { background: var(--accent-hover); }
.btn-danger { background: rgba(218, 54, 51, 0.15); color: #f85149; border-color: rgba(218, 54, 51, 0.3); }
.btn-danger:hover { background: var(--danger); color: #fff; }
.btn-sm { padding: 0.25rem 0.6rem; font-size: 0.75rem; }
.btn-block { width: 100%; padding: 0.75rem; font-size: 1rem; margin-top: 1rem; }
.actions-cell { display: flex; gap: 0.4rem; align-items: center; }
.empty-state { padding: 2.5rem; text-align: center; color: var(--text-muted); }
.pair-card { max-width: 420px; margin: 1.25rem auto 2rem; background: var(--card-bg); border: 1px solid var(--border); border-radius: var(--radius); padding: 2rem; }
.form-label { display: block; margin-bottom: 0.5rem; font-weight: 500; }
.code-input {
    width: 100%;
    padding: 0.75rem;
    font-size: 1.5rem;
    letter-spacing: 0.2em;
    text-align: center;
    font-family: monospace;
    background: #0d1117;
    border: 1px solid var(--border);
    border-radius: 6px;
    color: var(--text);
}
.code-input:focus { outline: none; border-color: var(--accent); }
.notice { background: rgba(56, 139, 253, 0.1); border: 1px solid rgba(56, 139, 253, 0.3); color: #58a6ff; padding: 1rem; border-radius: 6px; margin-bottom: 1.5rem; }
.desc { color: var(--text-muted); margin-bottom: 1.5rem; }

/* Image Studio Styles */
.image-studio {
    display: grid;
    grid-template-columns: 280px 1fr;
    gap: 1.5rem;
    padding: 1.25rem;
}
@media (max-width: 680px) {
    .image-studio { grid-template-columns: 1fr; }
}
.preview-pane {
    display: flex;
    flex-direction: column;
    align-items: center;
    background: #0d1117;
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 1rem;
}
.screen-frame {
    width: 240px;
    height: 320px;
    border-radius: 6px;
    background: #000;
    border: 2px solid #30363d;
    overflow: hidden;
    position: relative;
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 0 4px 16px rgba(0,0,0,0.5);
}
.screen-frame canvas {
    width: 240px;
    height: 320px;
    display: block;
}
.screen-placeholder {
    color: #484f58;
    text-align: center;
    font-size: 0.85rem;
    padding: 1rem;
}
.upload-pane {
    display: flex;
    flex-direction: column;
    gap: 1rem;
}
.push-form {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    padding: 1.25rem;
}
.push-grid {
    display: grid;
    grid-template-columns: 2fr 1fr;
    gap: 1rem;
}
@media (max-width: 560px) {
    .push-grid { grid-template-columns: 1fr; }
}
/* 表格长文本: 固定最大行高 + 省略号, hover 用 title 展示全文 */
.cell-clamp {
    display: -webkit-box;
    -webkit-line-clamp: 2;
    -webkit-box-orient: vertical;
    overflow: hidden;
    text-overflow: ellipsis;
    line-height: 1.4;
    word-break: break-word;
    white-space: normal;
}
.cell-clamp-1 {
    display: -webkit-box;
    -webkit-line-clamp: 1;
    -webkit-box-orient: vertical;
    overflow: hidden;
    text-overflow: ellipsis;
    word-break: break-word;
    white-space: normal;
}
.form-group { display: flex; flex-direction: column; gap: 0.35rem; }
.form-input, .form-select {
    background: #0d1117;
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.6rem 0.75rem;
    color: var(--text);
    font-size: 0.9rem;
}
.form-input:focus, .form-select:focus { outline: none; border-color: var(--accent); }
.gallery-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
    gap: 1rem;
    padding: 1.25rem;
}
.gallery-item {
    background: #0d1117;
    border: 1px solid var(--border);
    border-radius: 6px;
    overflow: hidden;
    display: flex;
    flex-direction: column;
    transition: border-color 0.2s ease, transform 0.2s ease;
}
.gallery-item:hover { border-color: #3d4650; transform: translateY(-2px); }
.gallery-thumb {
    width: 100%;
    height: 180px;
    object-fit: cover;
    background: #000;
}
.gallery-info {
    padding: 0.75rem;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    flex: 1;
}
.gallery-title { font-weight: 600; font-size: 0.85rem; word-break: break-all; }
.gallery-date { font-size: 0.75rem; color: var(--text-muted); }
.gallery-actions { display: flex; gap: 0.5rem; margin-top: auto; padding-top: 0.5rem; }

.footer { text-align: center; color: var(--text-muted); font-size: 0.78rem; padding: 2.5rem 1rem 1.5rem; }

/* ============ 视觉升级层 (Design System) ============ */
:root {
    --bg: #080a10;
    --card-bg: #0e1117;
    --border: rgba(148, 163, 184, 0.14);
    --text: #e7ecf3;
    --text-muted: #8b94a3;
    --accent: #6366f1;
    --accent-hover: #818cf8;
    --accent-soft: rgba(99, 102, 241, 0.16);
    --primary: #4f46e5;
    --primary-hover: #6366f1;
    --radius: 12px;
    --shadow-card: 0 1px 2px rgba(0,0,0,0.4), 0 8px 24px -12px rgba(0,0,0,0.6);
    --glow: 0 0 24px rgba(99, 102, 241, 0.25);
}
* { scrollbar-width: thin; scrollbar-color: rgba(148,163,184,0.3) transparent; }
::-webkit-scrollbar { width: 10px; height: 10px; }
::-webkit-scrollbar-thumb { background: rgba(148,163,184,0.25); border-radius: 8px; border: 2px solid transparent; background-clip: content-box; }
::-webkit-scrollbar-thumb:hover { background: rgba(148,163,184,0.4); border: 2px solid transparent; background-clip: content-box; }
::selection { background: rgba(99,102,241,0.35); color: #fff; }
body {
    background-color: var(--bg);
    background-image:
        radial-gradient(1100px 520px at 12% -12%, rgba(99,102,241,0.14), transparent 60%),
        radial-gradient(900px 480px at 92% -8%, rgba(6,182,212,0.10), transparent 55%),
        radial-gradient(700px 500px at 50% 115%, rgba(139,92,246,0.08), transparent 60%);
    letter-spacing: 0.01em;
}
.container { padding-top: 2.25rem; }
.topnav {
    background: rgba(8, 10, 16, 0.72);
    backdrop-filter: blur(18px) saturate(1.4);
    -webkit-backdrop-filter: blur(18px) saturate(1.4);
    border-bottom: 1px solid rgba(148,163,184,0.12);
    box-shadow: 0 1px 0 rgba(255,255,255,0.04) inset;
}
.brand { font-size: 1.08rem; }
.brand span {
    background: linear-gradient(92deg, #818cf8, #c084fc 55%, #22d3ee);
    -webkit-background-clip: text;
    background-clip: text;
    -webkit-text-fill-color: transparent;
}
.nav-tab { border-radius: 9px; font-weight: 550; letter-spacing: 0.01em; }
.nav-tab:hover { background: rgba(255,255,255,0.07); color: var(--text); transform: translateY(-1px); }
.nav-tab.active {
    background: linear-gradient(180deg, rgba(99,102,241,0.22), rgba(99,102,241,0.10));
    border-color: rgba(129,140,248,0.35);
    color: #c7d2fe;
    box-shadow: 0 0 16px rgba(99,102,241,0.18) inset, 0 0 20px rgba(99,102,241,0.10);
}
.nav-btn { background: rgba(148,163,184,0.08); border-color: rgba(148,163,184,0.16); }
.nav-btn:hover { background: rgba(148,163,184,0.14); border-color: rgba(148,163,184,0.3); }
.page-head h1 { font-size: 1.5rem; letter-spacing: -0.02em; }

/* 页面切换: 子元素依次上浮 (轻量 CSS 动画) */
.page.active > * { animation: fadeUp 0.5s cubic-bezier(0.22, 1, 0.36, 1) both; }
.page.active > *:nth-child(2) { animation-delay: 0.05s; }
.page.active > *:nth-child(3) { animation-delay: 0.10s; }
.page.active > *:nth-child(4) { animation-delay: 0.15s; }
.page.active > *:nth-child(5) { animation-delay: 0.20s; }
.page.active > *:nth-child(6) { animation-delay: 0.25s; }
@keyframes fadeUp {
    from { opacity: 0; transform: translateY(10px); }
    to { opacity: 1; transform: none; }
}

/* 卡片 */
.card {
    background: linear-gradient(180deg, rgba(255,255,255,0.035), rgba(255,255,255,0.012)), var(--card-bg);
    border: 1px solid rgba(148,163,184,0.12);
    border-radius: 14px;
    box-shadow: var(--shadow-card);
    transition: transform 0.22s cubic-bezier(0.22,1,0.36,1), border-color 0.22s ease, box-shadow 0.22s ease;
}
.card:hover { border-color: rgba(148,163,184,0.24); box-shadow: 0 2px 4px rgba(0,0,0,0.4), 0 16px 40px -16px rgba(0,0,0,0.7); }
.card-header { padding: 1.05rem 1.3rem; background: rgba(255,255,255,0.015); }
.card-title { font-size: 1.05rem; letter-spacing: -0.01em; }

/* 统计卡片 */
.stat-card {
    background: linear-gradient(160deg, rgba(255,255,255,0.06), rgba(255,255,255,0.015)), #0e1117;
    border: 1px solid rgba(148,163,184,0.12);
    border-radius: 14px;
    box-shadow: var(--shadow-card);
}
.stat-card::after {
    height: 2.5px;
    background: linear-gradient(90deg, rgba(99,102,241,0.9), rgba(34,211,238,0.5), transparent 80%);
    opacity: 1;
}
.stat-card:hover { transform: translateY(-3px); box-shadow: var(--shadow-card), var(--glow); }
.stat-icon {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 42px;
    height: 42px;
    border-radius: 11px;
    font-size: 1.25rem;
    background: linear-gradient(140deg, rgba(99,102,241,0.25), rgba(34,211,238,0.12));
    border: 1px solid rgba(129,140,248,0.25);
    box-shadow: 0 0 18px rgba(99,102,241,0.15) inset;
    margin-bottom: 0.9rem;
}
.stat-label { text-transform: uppercase; font-size: 0.72rem; letter-spacing: 0.08em; }
.stat-value { font-size: 2rem; font-variant-numeric: tabular-nums; letter-spacing: -0.03em; }

/* 快捷操作 */
.quick-card {
    border-radius: 14px;
    background: linear-gradient(180deg, rgba(255,255,255,0.035), rgba(255,255,255,0.01)), var(--card-bg);
    box-shadow: var(--shadow-card);
}
.quick-card:hover {
    transform: translateY(-3px);
    border-color: rgba(129,140,248,0.45);
    box-shadow: var(--shadow-card), 0 0 28px rgba(99,102,241,0.12);
}
.quick-icon {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 46px;
    height: 46px;
    border-radius: 12px;
    font-size: 1.5rem;
    background: linear-gradient(140deg, rgba(99,102,241,0.18), rgba(6,182,212,0.10));
    border: 1px solid rgba(148,163,184,0.16);
}

/* 按钮 */
.btn { border-radius: 9px; font-weight: 550; }
.btn:active { transform: translateY(1px) scale(0.99); }
.btn-primary {
    background: linear-gradient(135deg, #4f46e5, #7c3aed);
    border-color: rgba(255,255,255,0.14);
    box-shadow: 0 4px 16px -4px rgba(79,70,229,0.55), 0 1px 2px rgba(0,0,0,0.3);
}
.btn-primary:hover {
    background: linear-gradient(135deg, #6366f1, #8b5cf6);
    box-shadow: 0 6px 20px -4px rgba(99,102,241,0.65);
    transform: translateY(-1px);
}
.btn-accent { background: linear-gradient(135deg, #0ea5e9, #6366f1); }
.btn-accent:hover { transform: translateY(-1px); box-shadow: 0 6px 20px -4px rgba(14,165,233,0.5); }
.btn-danger:hover { transform: translateY(-1px); }
.btn:focus-visible, .nav-tab:focus-visible, .quick-card:focus-visible, a:focus-visible, button:focus-visible {
    outline: 2px solid rgba(129,140,248,0.7);
    outline-offset: 2px;
}

/* 表格 */
th { background: rgba(148,163,184,0.06); color: var(--text-muted); font-size: 0.74rem; text-transform: uppercase; letter-spacing: 0.06em; }
tr:hover td { background: rgba(129,140,248,0.05); }
td { transition: background 0.15s ease; }

/* 徽章 / 状态点 */
.badge { border-radius: 999px; }
.badge-active { color: #4ade80; background: rgba(34,197,94,0.12); border-color: rgba(34,197,94,0.28); }
.badge-revoked { color: #f87171; background: rgba(239,68,68,0.12); border-color: rgba(239,68,68,0.28); }
.dot { position: relative; }
.dot-online { background: #34d399; box-shadow: 0 0 8px #34d399; animation: pulseDot 2s ease infinite; }
@keyframes pulseDot {
    0%, 100% { box-shadow: 0 0 0 0 rgba(52,211,153,0.45); }
    50% { box-shadow: 0 0 0 5px rgba(52,211,153,0); }
}

/* 表单 */
.form-input, .form-select {
    background: rgba(255,255,255,0.03);
    border-color: rgba(148,163,184,0.18);
    border-radius: 9px;
    transition: border-color 0.15s ease, box-shadow 0.15s ease, background 0.15s ease;
}
.form-input:hover, .form-select:hover { border-color: rgba(148,163,184,0.32); }
.form-input:focus, .form-select:focus {
    border-color: rgba(129,140,248,0.65);
    box-shadow: 0 0 0 3px rgba(99,102,241,0.15);
    background: rgba(255,255,255,0.045);
}

/* 推送结果提示弹出动画 */
#pushResult, #hookStatus, #statusAlert, #castAlert { animation: popIn 0.28s cubic-bezier(0.22, 1, 0.36, 1); }
@keyframes popIn {
    from { opacity: 0; transform: translateY(6px) scale(0.98); }
    to { opacity: 1; transform: none; }
}

/* 屏幕预览框光晕 + 图库 */
.screen-frame { border-color: rgba(148,163,184,0.25); box-shadow: 0 0 0 1px rgba(148,163,184,0.08), 0 12px 32px -8px rgba(0,0,0,0.7), 0 0 32px rgba(99,102,241,0.08); }
.gallery-item { border-radius: 12px; box-shadow: 0 2px 10px -4px rgba(0,0,0,0.5); }
.gallery-item:hover { transform: translateY(-3px); box-shadow: 0 10px 26px -8px rgba(0,0,0,0.7); }
.footer { color: var(--text-muted); opacity: 0.8; }

/* 设备心跳监控 */
.monitor-stats {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
    gap: 0.9rem;
    padding: 1.1rem 1.25rem 0.4rem;
}
.monitor-stats .stat-card { padding: 1rem; }
.monitor-chart { padding: 0.8rem 1.25rem 0.4rem; }
.monitor-chart canvas {
    width: 100%;
    height: 160px;
    display: block;
    border-radius: 10px;
    background: rgba(255,255,255,0.02);
    border: 1px solid rgba(148,163,184,0.1);
}
.chart-hint { font-size: 0.72rem; color: var(--text-muted); margin-top: 0.4rem; text-align: center; }
.monitor-body {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 1.2rem;
    padding: 0.8rem 1.25rem 1.25rem;
}
@media (max-width: 680px) {
    .monitor-body { grid-template-columns: 1fr; }
}
.monitor-subtitle {
    font-size: 0.74rem;
    font-weight: 600;
    color: var(--text-muted);
    text-transform: uppercase;
    letter-spacing: 0.05em;
    margin-bottom: 0.5rem;
}
.mon-device-row, .mon-event-row {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    padding: 0.42rem 0.5rem;
    border-radius: 8px;
    font-size: 0.82rem;
}
.mon-device-row:hover, .mon-event-row:hover { background: rgba(129,140,248,0.06); }
.mon-uptime-bar {
    flex: 1;
    min-width: 60px;
    height: 6px;
    border-radius: 4px;
    background: rgba(148,163,184,0.14);
    overflow: hidden;
}
.mon-uptime-fill {
    height: 100%;
    border-radius: 4px;
    background: linear-gradient(90deg, #6366f1, #22d3ee);
    transition: width 0.4s ease;
}
.mon-pct {
    width: 48px;
    text-align: right;
    font-variant-numeric: tabular-nums;
    color: var(--text-muted);
}
.mon-event-badge {
    padding: 0.08rem 0.45rem;
    border-radius: 999px;
    font-size: 0.7rem;
    font-weight: 600;
    white-space: nowrap;
}
.mon-event-online { background: rgba(34,197,94,0.14); color: #4ade80; border: 1px solid rgba(34,197,94,0.3); }
.mon-event-offline { background: rgba(239,68,68,0.14); color: #f87171; border: 1px solid rgba(239,68,68,0.3); }

/* 减少动态效果偏好 */
@media (prefers-reduced-motion: reduce) {
    *, *::before, *::after { animation-duration: 0.01ms !important; animation-iteration-count: 1 !important; transition-duration: 0.01ms !important; }
}
</style>
</head>
<body>
${nav}
<div class="container">
    ${content}
</div>
<footer class="footer">Kiro Passport Relay · Cloudflare Worker · ws.yanyun.asia</footer>
</body>
</html>`;
    const headers = new Headers({
        "Content-Type": "text/html; charset=utf-8",
        "Cache-Control": "no-store",
        "Content-Security-Policy": "default-src 'none'; connect-src 'self' ws: wss:; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'; img-src 'self' data:; script-src 'unsafe-inline'",
        "Referrer-Policy": "no-referrer",
        "X-Content-Type-Options": "nosniff",
    });
    for (const [name, value] of new Headers(additionalHeaders)) headers.set(name, value);
    return new Response(body, { status, headers });
}

function escapeHtml(value: string): string {
    return value.replaceAll(/[&<>"']/gu, (character) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[character] ?? character);
}

type DeviceWithOnline = DeviceRow & { online: boolean };

function renderHookLogRows(hookLogs: HookNotifyLogRow[]): string {
    if (hookLogs.length === 0) {
        return `<tr><td colspan="6" class="empty-state">暂无 hook 推送记录。Agent 结束或 bridge notify 触发后会在此显示。</td></tr>`;
    }
    let rows = "";
    for (const log of hookLogs) {
        const resultBadge = log.result === "sent"
            ? `<span class="badge badge-active">✓ 已送达</span>`
            : log.result === "offline"
                ? `<span class="badge" style="color:#d29922;border:1px solid rgba(210,153,34,0.3);background:rgba(210,153,34,0.1);">● 离线</span>`
                : `<span class="badge" style="color:#f85149;border:1px solid rgba(248,81,73,0.3);background:rgba(248,81,73,0.1);">✗ 失败</span>`;
        rows += `
        <tr>
            <td><span class="code-mono">${escapeHtml(log.device_id)}</span></td>
            <td>${formatDate(log.created_at)}</td>
            <td class="cell-clamp-1" title="${escapeHtml(log.title)}" style="max-width:180px;">${escapeHtml(log.title)}</td>
            <td class="cell-clamp" title="${escapeHtml(log.content)}" style="max-width:300px;"><span style="color:var(--text-muted);">${escapeHtml(log.content)}</span></td>
            <td>${log.online ? '🟢 在线' : '⚪ 离线'}</td>
            <td>${resultBadge}</td>
        </tr>`;
    }
    return rows;
}

function renderDeviceOptions(deviceList: DeviceWithOnline[]): string {
    if (deviceList.length === 0) return `<option value="">请先配对设备</option>`;
    let options = "";
    for (const d of deviceList) {
        options += `<option value="${escapeHtml(d.device_id)}">${escapeHtml(d.device_id)} (${d.online ? '🟢 在线' : '⚪ 离线'})</option>`;
    }
    return options;
}

function renderDeviceRows(deviceList: DeviceWithOnline[]): string {
    if (deviceList.length === 0) {
        return `<tr><td colspan="6" class="empty-state">暂无已绑定的设备。点击上方「配对新设备」开始添加。</td></tr>`;
    }
    let rows = "";
    for (const d of deviceList) {
        const statusBadge = d.status === "active"
            ? `<span class="badge badge-active">Active</span>`
            : `<span class="badge badge-revoked">Revoked</span>`;
        const onlineDot = d.online
            ? `<span class="badge badge-active"><span class="dot dot-online"></span> 在线</span>`
            : `<span class="badge" style="color: var(--text-muted);"><span class="dot dot-offline"></span> 离线</span>`;
        rows += `
        <tr>
            <td><span class="code-mono">${escapeHtml(d.device_id)}</span></td>
            <td>${onlineDot}</td>
            <td>${statusBadge}</td>
            <td>v${d.credential_version}</td>
            <td>${formatDate(d.created_at)}</td>
            <td>
                <div class="actions-cell">
                    ${d.status === "active" ? `
                    <form method="post" action="/admin/devices/revoke" onsubmit="return confirm('确定要撤销设备 ${escapeHtml(d.device_id)} 吗？');">
                        <input type="hidden" name="device_id" value="${escapeHtml(d.device_id)}">
                        <button type="submit" class="btn btn-sm btn-danger">撤销</button>
                    </form>` : ""}
                    <form method="post" action="/admin/devices/delete" onsubmit="return confirm('确定要彻底删除设备 ${escapeHtml(d.device_id)} 吗？');">
                        <input type="hidden" name="device_id" value="${escapeHtml(d.device_id)}">
                        <button type="submit" class="btn btn-sm" style="background: #21262d; color: var(--text-muted); border: 1px solid var(--border);">删除</button>
                    </form>
                </div>
            </td>
        </tr>`;
    }
    return rows;
}

function renderPushLogRows(pushLogs: ApprovalLogRow[]): string {
    if (pushLogs.length === 0) {
        return `<tr><td colspan="6" class="empty-state">暂无审批推送记录。使用上方表单向设备推送第一条审批请求。</td></tr>`;
    }
    const reasonLabels: Record<string, string> = {
        user: "用户拒绝",
        policy: "策略拒绝",
        timeout: "超时未决定",
        offline: "设备离线",
        session_lost: "连接断开",
        protocol_error: "协议错误",
    };
    let rows = "";
    for (const log of pushLogs) {
        let badge: string;
        if (log.status === "allow") badge = `<span class="badge badge-active">✅ 已批准</span>`;
        else if (log.status === "deny") badge = `<span class="badge badge-revoked">❌ 已拒绝</span>`;
        else badge = `<span class="badge" style="background: rgba(31,111,235,0.15); color: #58a6ff; border: 1px solid rgba(31,111,235,0.3);">⏳ 待审批</span>`;
        const resultText = log.status === "pending"
            ? "等待设备决定"
            : (reasonLabels[log.reason ?? ""] ?? log.reason ?? "—");
        rows += `
        <tr>
            <td><span class="code-mono" style="font-size: 0.75rem;">${formatDate(log.created_at)}</span></td>
            <td><span class="code-mono">${escapeHtml(log.device_id)}</span></td>
            <td><span class="code-mono">${escapeHtml(log.tool)}</span></td>
            <td title="${escapeHtml(log.summary)}" style="max-width: 220px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;">${escapeHtml(log.summary)}</td>
            <td>${badge}</td>
            <td style="color: var(--text-muted); font-size: 0.8rem;">${escapeHtml(resultText)}</td>
        </tr>`;
    }
    return rows;
}

async function loadDashboardData(env: Env): Promise<{
    totalCount: number;
    onlineCount: number;
    activeCount: number;
    pendingCount: number;
    deviceList: DeviceWithOnline[];
    pushLogs: ApprovalLogRow[];
    hookLogs: HookNotifyLogRow[];
}> {
    const devices = (await env.DB.prepare(
        "SELECT device_id, status, credential_version, created_at, rotated_at FROM devices ORDER BY created_at DESC"
    ).all<DeviceRow>()).results;
    const now = nowSeconds();
    const pendings = (await env.DB.prepare(
        "SELECT enrollment_id, device_id, status, expires_at, created_at FROM device_enrollments WHERE status = 'pending' AND expires_at > ?1 ORDER BY created_at DESC"
    ).bind(now).all<PendingEnrollmentRow>()).results;
    const pushLogs = (await env.DB.prepare(
        "SELECT request_id, device_id, tool, summary, status, reason, created_at, decided_at, expires_at " +
        "FROM approval_requests ORDER BY created_at DESC LIMIT 50"
    ).all<ApprovalLogRow>()).results;
    const deviceList = await Promise.all(devices.map(async (d) => {
        const online = await checkDeviceOnline(env, d.device_id);
        return { ...d, online };
    }));
    const hookLogs = await listHookNotifyLogs(env, { limit: 50 }).catch(() => []);
    return {
        totalCount: deviceList.length,
        onlineCount: deviceList.filter((d) => d.online).length,
        activeCount: deviceList.filter((d) => d.status === "active").length,
        pendingCount: pendings.length,
        deviceList,
        pushLogs,
        hookLogs,
    };
}

async function adminDashboardData(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const data = await loadDashboardData(env);
    return json({
        stats: {
            total: data.totalCount,
            online: data.onlineCount,
            active: data.activeCount,
            pending: data.pendingCount,
        },
        deviceRows: renderDeviceRows(data.deviceList),
        pushLogRows: renderPushLogRows(data.pushLogs),
        hookLogRows: renderHookLogRows(data.hookLogs),
        ts: nowSeconds(),
    });
}

interface MonitorDevice {
    device_id: string;
    online_now: boolean;
    last_seen: number | null;
    online_events_24h: number;
    offline_events_24h: number;
    uptime_pct_24h: number;
}

/** 由时间先后排列的在线/离线事件推导出 [since, now] 窗口内的在线区间。 */
function onlineIntervals(events: DeviceEventRow[], since: number, now: number): Array<[number, number]> {
    const sorted = [...events].sort((a, b) => a.created_at - b.created_at);
    const intervals: Array<[number, number]> = [];
    let start: number | null = null;
    for (const ev of sorted) {
        if (ev.event === "online") {
            start = ev.created_at;
        } else if (ev.event === "offline" && start !== null) {
            intervals.push([Math.max(start, since), Math.min(ev.created_at, now)]);
            start = null;
        }
    }
    if (start !== null) intervals.push([Math.max(start, since), now]);
    return intervals;
}

function intervalOverlap(a: [number, number], b: [number, number]): number {
    const start = Math.max(a[0], b[0]);
    const end = Math.min(a[1], b[1]);
    return end > start ? end - start : 0;
}

const MONITOR_WINDOW_SECONDS = 24 * 3600;

async function adminMonitoringWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);

    const now = nowSeconds();
    const since = now - MONITOR_WINDOW_SECONDS;

    const devices = (await env.DB.prepare(
        "SELECT device_id, status FROM devices ORDER BY created_at DESC"
    ).all<{ device_id: string; status: string }>()).results;
    const deviceList = await Promise.all(devices.map(async (d) => {
        const online = await checkDeviceOnline(env, d.device_id);
        return { device_id: d.device_id, online_now: online };
    }));
    const onlineNowIds = new Set(deviceList.filter((d) => d.online_now).map((d) => d.device_id));

    const events = await listDeviceEvents(env, { since });
    const byDevice = new Map<string, DeviceEventRow[]>();
    for (const ev of events) {
        if (!byDevice.has(ev.device_id)) byDevice.set(ev.device_id, []);
        byDevice.get(ev.device_id)!.push(ev);
    }

    const monitorDevices: MonitorDevice[] = devices.map((d) => {
        const evs = byDevice.get(d.device_id) ?? [];
        const intervals = onlineIntervals(evs, since, now);
        let onlineSeconds = 0;
        for (const iv of intervals) onlineSeconds += iv[1] - iv[0];
        return {
            device_id: d.device_id,
            online_now: onlineNowIds.has(d.device_id),
            last_seen: evs.length ? evs[0].created_at : null,
            online_events_24h: evs.filter((e) => e.event === "online").length,
            offline_events_24h: evs.filter((e) => e.event === "offline").length,
            uptime_pct_24h: Math.round((onlineSeconds / MONITOR_WINDOW_SECONDS) * 1000) / 10,
        };
    });

    // 按小时统计舰队在线率（24 个桶）
    const deviceCount = Math.max(1, devices.length);
    const hourlySeconds = new Array<number>(24).fill(0);
    for (const d of devices) {
        const evs = byDevice.get(d.device_id) ?? [];
        const intervals = onlineIntervals(evs, since, now);
        for (let h = 0; h < 24; h++) {
            const hs = now - (24 - h) * 3600;
            let s = 0;
            for (const iv of intervals) s += intervalOverlap(iv, [hs, hs + 3600]);
            hourlySeconds[h] += s;
        }
    }
    const hourly = hourlySeconds.map((s) => Math.round((s / 3600 / deviceCount) * 1000) / 10);

    const fleetUptime = devices.length === 0
        ? 0
        : Math.round((monitorDevices.reduce((sum, d) => sum + d.uptime_pct_24h, 0) / devices.length) * 10) / 10;

    return json({
        now,
        fleet: {
            total: devices.length,
            online_now: deviceList.filter((d) => d.online_now).length,
            uptime_pct_24h: fleetUptime,
            offline_events_24h: monitorDevices.reduce((sum, d) => sum + d.offline_events_24h, 0),
        },
        hourly,
        devices: monitorDevices,
        recent_events: events.slice(0, 30).map((e) => ({ device_id: e.device_id, event: e.event, created_at: e.created_at })),
    });
}

async function adminDashboardPage(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return adminUnauthorizedPage();

    const images = (await env.DB.prepare(
        "SELECT image_id, device_id, title, image_data, created_at FROM device_images ORDER BY created_at DESC LIMIT 12"
    ).all<DeviceImageRow>()).results;

    const data = await loadDashboardData(env);
    const { totalCount, onlineCount, activeCount, pendingCount } = data;
    const pushLogs = data.pushLogs;
    const hookLogs = data.hookLogs;
    const devicesRows = renderDeviceRows(data.deviceList);
    const deviceOptions = renderDeviceOptions(data.deviceList);
    const hookLogsRows = renderHookLogRows(data.hookLogs);
    const pushLogRows = renderPushLogRows(data.pushLogs);

    let galleryHtml = "";
    if (images.length === 0) {
        galleryHtml = `<div class="empty-state" style="grid-column: 1/-1;">暂无已推送的图片历史。请在上方选择图片并发送。</div>`;
    } else {
        for (const img of images) {
            const dataUrl = `data:image/jpeg;base64,${img.image_data}`;
            galleryHtml += `
            <div class="gallery-item">
                <img src="${dataUrl}" class="gallery-thumb" alt="${escapeHtml(img.title || 'Image')}">
                <div class="gallery-info">
                    <div class="gallery-title">${escapeHtml(img.title || '未命名图片')}</div>
                    <div class="gallery-date"><span class="code-mono" style="font-size:0.75rem;">${escapeHtml(img.device_id)}</span></div>
                    <div class="gallery-date">${formatDate(img.created_at)}</div>
                    <div class="gallery-actions">
                        <button type="button" class="btn btn-sm btn-primary" onclick="repushImage('${escapeHtml(img.device_id)}', '${escapeHtml(img.title || 'Image')}', '${img.image_data}')">🚀 重新推送</button>
                        <button type="button" class="btn btn-sm btn-danger" onclick="deleteImage('${img.image_id}')">删除</button>
                    </div>
                </div>
            </div>`;
        }
    }

    const content = `
        <section class="page active" data-page="overview">
            <div class="page-head">
                <h1>📊 概览</h1>
                <p class="page-desc">已登录 <strong>${escapeHtml(username)}</strong> · Kiro Passport Relay 运行总览</p>
            </div>

            <div class="stats-grid">
                <div class="stat-card">
                    <div class="stat-icon">📱</div>
                    <div class="stat-label">总设备数</div>
                    <div class="stat-value" id="statTotal">${totalCount}</div>
                </div>
                <div class="stat-card">
                    <div class="stat-icon">🟢</div>
                    <div class="stat-label">在线设备</div>
                    <div class="stat-value" id="statOnline" style="color: var(--online);">${onlineCount}</div>
                </div>
                <div class="stat-card">
                    <div class="stat-icon">✅</div>
                    <div class="stat-label">活跃设备</div>
                    <div class="stat-value" id="statActive">${activeCount}</div>
                </div>
                <div class="stat-card">
                    <div class="stat-icon">⏳</div>
                    <div class="stat-label">待处理配对</div>
                    <div class="stat-value" id="statPending" style="color: #58a6ff;">${pendingCount}</div>
                </div>
            </div>

            <h2 class="section-title">快速操作</h2>
            <div class="quick-grid">
                <a href="#" class="quick-card" onclick="switchTab('snapshot'); return false;">
                    <span class="quick-icon">📸</span>
                    <div>
                        <div class="quick-title">远程屏幕快照</div>
                        <div class="quick-desc">实时抓取设备 240×320 屏幕</div>
                    </div>
                </a>
                <a href="#" class="quick-card" onclick="switchTab('images'); return false;">
                    <span class="quick-icon">🖼️</span>
                    <div>
                        <div class="quick-title">推送图片</div>
                        <div class="quick-desc">缩放裁剪并推送壁纸封面</div>
                    </div>
                </a>
                <a href="#" class="quick-card" onclick="switchTab('requests'); return false;">
                    <span class="quick-icon">🔔</span>
                    <div>
                        <div class="quick-title">审批推送</div>
                        <div class="quick-desc">向设备推送审批请求并查看日志</div>
                    </div>
                </a>
                <a href="#" class="quick-card" onclick="switchTab('notify'); return false;">
                    <span class="quick-icon">📣</span>
                    <div>
                        <div class="quick-title">通知推送</div>
                        <div class="quick-desc">向设备屏幕推送通知（支持中文）</div>
                    </div>
                </a>
                <a href="/admin/pair" class="quick-card">
                    <span class="quick-icon">➕</span>
                    <div>
                        <div class="quick-title">配对新设备</div>
                        <div class="quick-desc">输入设备 6 位配对码</div>
                    </div>
                </a>
                <a href="#" class="quick-card" onclick="switchTab('devices'); return false;">
                    <span class="quick-icon">🔐</span>
                    <div>
                        <div class="quick-title">设备管理</div>
                        <div class="quick-desc">撤销授权 / 轮换凭证 / 删除</div>
                    </div>
                </a>
            </div>
        </section>

        <!-- 📸 远程屏幕快照与截屏 (Remote Screen Capture Studio) -->
        <section class="page" data-page="snapshot">
            <div class="page-head">
                <h1>📸 远程屏幕快照</h1>
                <p class="page-desc">实时抓取设备屏幕（240×320），支持自动轮询与保存高清截图</p>
            </div>
            <div class="card">
                <div class="card-header">
                    <div class="card-title">远程屏幕快照与截屏 (Remote Snapshot - 240×320)</div>
                    <div id="castStatusBadge" class="badge" style="background: rgba(110, 118, 129, 0.2); color: var(--text-muted);">
                        <span id="castStatusDot" class="dot dot-offline"></span> <span id="castStatusText">未连接</span>
                    </div>
                </div>
                <div class="image-studio">
                    <div class="preview-pane">
                        <div class="screen-frame" id="castFrame">
                            <canvas id="castCanvas" width="240" height="320" style="image-rendering: pixelated;"></canvas>
                            <div id="castPlaceholder" class="screen-placeholder" style="position: absolute; top:0; left:0; width:100%; height:100%; display:flex; flex-direction:column; align-items:center; justify-content:center; background: rgba(13,17,23,0.85);">
                                📱 板子屏幕快照<br><span style="font-size:0.75rem; color:#6e7681; margin-top:0.5rem;">点击「立即抓取屏幕快照」</span>
                            </div>
                        </div>
                        <div id="castMetrics" style="font-size: 0.8rem; color: var(--text-muted); margin-top: 0.75rem; display: flex; gap: 0.75rem;">
                            <span id="castSliceCount">0/64 片</span>
                            <span>•</span>
                            <span id="castSeq">帧 #0</span>
                            <span>•</span>
                            <span id="castTime">未获取</span>
                        </div>
                    </div>
                    <div class="upload-pane">
                        <div class="form-group">
                            <label class="form-label">选择目标设备</label>
                            <select id="castTargetDevice" class="form-select">${deviceOptions}</select>
                        </div>
                        <div style="display: flex; gap: 0.5rem; flex-wrap: wrap;">
                            <button type="button" id="castCaptureBtn" class="btn btn-primary" style="flex: 1; padding: 0.85rem; font-size: 1rem; font-weight: 600;" onclick="requestCapture()">
                                📸 立即抓取屏幕快照
                            </button>
                        </div>
                        <div style="display: flex; align-items: center; gap: 0.5rem; margin: 0.25rem 0;">
                            <label style="display: flex; align-items: center; gap: 0.4rem; font-size: 0.85rem; color: var(--text-muted); cursor: pointer;">
                                <input type="checkbox" id="autoSnapshotCheck" onchange="toggleAutoSnapshot()">
                                <span>⏱️ 自动轮询快照 (每 5 秒刷新一次)</span>
                            </label>
                        </div>
                        <div style="display: flex; gap: 0.5rem;">
                            <button type="button" class="btn" style="background: #21262d; border: 1px solid var(--border); color: var(--text); flex: 1;" onclick="downloadScreenshot()">
                                💾 保存高清截图 (PNG)
                            </button>
                            <button type="button" class="btn" style="background: #21262d; border: 1px solid var(--border); color: var(--text-muted);" onclick="reconnectCastWs()">
                                🔄 刷新连接
                            </button>
                        </div>
                        <div id="castAlert" style="display:none; padding: 0.75rem; border-radius: 6px; font-size: 0.85rem;"></div>
                    </div>
                </div>
            </div>
        </section>

        <!-- 🖼️ 图片推送工作台 (Image Studio) -->
        <section class="page" data-page="images">
            <div class="page-head">
                <h1>🖼️ 图片推送工作台</h1>
                <p class="page-desc">将本地图片缩放裁剪至 240×320 并实时推送到设备屏幕</p>
            </div>
            <div class="card">
                <div class="card-header">
                    <div class="card-title">图片推送 (Image Studio - 240×320)</div>
                </div>
                <div class="image-studio">
                    <div class="preview-pane">
                        <div class="screen-frame" id="screenFrame">
                            <div class="screen-placeholder" id="placeholder">
                                📱 240 × 320 预览<br><span style="font-size:0.75rem; color:#6e7681;">请选择要推送的图片</span>
                            </div>
                            <canvas id="previewCanvas" width="240" height="320" style="display:none;"></canvas>
                        </div>
                        <div id="sizeBadge" style="font-size: 0.8rem; color: var(--text-muted); margin-top: 0.75rem;">大小: 0 KB</div>
                    </div>
                    <div class="upload-pane">
                        <div class="form-group">
                            <label class="form-label">目标设备</label>
                            <select id="targetDevice" class="form-select">${deviceOptions}</select>
                        </div>
                        <div class="form-group">
                            <label class="form-label">图片标题 / 描述</label>
                            <input id="imageTitle" type="text" class="form-input" placeholder="例如：今日封面、日程安排" maxlength="32" value="壁纸封面">
                        </div>
                        <div class="form-group">
                            <label class="form-label">选择本地图片 (自动缩放裁剪至 240×320)</label>
                            <input id="filePicker" type="file" accept="image/*" class="form-input">
                        </div>
                        <button type="button" id="pushBtn" class="btn btn-primary" style="padding: 0.75rem; font-size: 1rem;" onclick="pushCurrentCanvas()">
                            🚀 立即推送到设备屏幕
                        </button>
                        <div id="statusAlert" style="display:none; padding: 0.75rem; border-radius: 6px; font-size: 0.85rem;"></div>
                    </div>
                </div>
            </div>
            <div class="card">
                <div class="card-header">
                    <div class="card-title">🖼️ 历史图片库</div>
                </div>
                <div class="gallery-grid">
                    ${galleryHtml}
                </div>
            </div>
        </section>

        <!-- 📟 信息壁纸 (Info Wallpaper) -->
        <section class="page" data-page="wallpaper">
            <div class="page-head">
                <h1>📟 信息壁纸</h1>
                <p class="page-desc">云端生成 240×320 信息屏（大时钟 + 天气 + 日程备注），经图片通道推送到设备 · 每小时自动更新</p>
            </div>
            <div class="card">
                <div class="card-header">
                    <div class="card-title">实时预览</div>
                    <div class="header-actions">
                        <button type="button" class="btn btn-sm nav-btn" onclick="refreshWallpaper()">🔄 刷新预览</button>
                        <button type="button" class="btn btn-sm btn-primary" onclick="pushWallpaper()">🚀 推送到设备</button>
                    </div>
                </div>
                <div class="image-studio">
                    <div class="preview-pane">
                        <div class="screen-frame" style="width:240px;height:320px;">
                            <img id="wallpaperImg" alt="wallpaper preview" style="width:240px;height:320px;object-fit:cover;display:block;">
                        </div>
                    </div>
                    <div class="upload-pane">
                        <div class="form-group">
                            <label class="form-label">目标设备（留空 = 全部在线设备）</label>
                            <select id="wallpaperDevice" class="form-select">${deviceOptions}</select>
                        </div>
                        <div class="form-group">
                            <label class="form-label">今日日程 / 备注（最多 3 行，建议英文数字）</label>
                            <textarea id="wallpaperNotes" class="form-input" rows="4" placeholder="Water plants&#10;Call mom&#10;Read book"></textarea>
                        </div>
                        <button type="button" class="btn" style="background:#21262d;border:1px solid var(--border);" onclick="saveWallpaperNotes()">💾 保存备注</button>
                        <div id="wallpaperStatus" style="display:none;padding:0.75rem;border-radius:6px;font-size:0.85rem;"></div>
                        <p class="desc" style="font-size:0.78rem;margin:0.4rem 0 0;">壁纸内容：大时钟 + 日期 + 天气（Open-Meteo 免费接口，城市在 WALLPAPER_CITY 配置）+ 备注。每小时整点自动推送一次。</p>
                    </div>
                </div>
            </div>
        </section>

        <!-- 🔔 审批推送 (Approval Push Center) -->
        <section class="page" data-page="requests">
            <div class="page-head">
                <h1>🔔 审批推送</h1>
                <p class="page-desc">向在线设备推送审批请求（tool/summary 显示在设备屏幕，用户按键决定），并查看全部推送日志</p>
            </div>
            <div class="card">
                <div class="card-header">
                    <div class="card-title">在线发推送 (Approval Push)</div>
                </div>
                <div class="push-form">
                    <div class="form-group">
                        <label class="form-label">目标设备</label>
                        <select id="pushDevice" class="form-select">${deviceOptions}</select>
                    </div>
                    <div class="push-grid">
                        <div class="form-group">
                            <label class="form-label">工具名称 (tool)</label>
                            <input id="pushTool" type="text" class="form-input" maxlength="31" placeholder="shell.execute" value="shell.execute" spellcheck="false">
                        </div>
                        <div class="form-group">
                            <label class="form-label">有效期 (TTL)</label>
                            <select id="pushTtl" class="form-select">
                                <option value="60">60 秒</option>
                                <option value="120" selected>120 秒</option>
                                <option value="300">300 秒</option>
                            </select>
                        </div>
                    </div>
                    <div class="form-group">
                        <label class="form-label">推送内容 (summary)</label>
                        <input id="pushSummary" type="text" class="form-input" maxlength="71" placeholder="Run deployment command" spellcheck="false">
                        <p class="desc" style="margin: 0.35rem 0 0; font-size: 0.78rem;">提示：内容需为可打印 ASCII 字符（不含引号 / 反斜杠），设备屏幕会原样显示。</p>
                    </div>
                    <button type="button" id="pushSendBtn" class="btn btn-primary" style="padding: 0.75rem; font-size: 1rem;" onclick="sendAdminPush()">
                        🚀 立即推送审批请求
                    </button>
                    <div id="pushResult" style="display:none; padding: 0.75rem; border-radius: 6px; font-size: 0.85rem; margin-top: 0.5rem;"></div>
                </div>
            </div>
            <div class="card">
                <div class="card-header">
                    <div class="card-title">📋 推送日志 (最近 ${pushLogs.length} 条)</div>
                </div>
                <table>
                    <thead>
                        <tr>
                            <th>时间</th>
                            <th>设备 ID</th>
                            <th>工具</th>
                            <th>摘要</th>
                            <th>状态</th>
                            <th>结果</th>
                        </tr>
                    </thead>
                    <tbody id="pushLogBody">${pushLogRows}</tbody>
                </table>
            </div>
        </section>

        <!-- 📱 已注册设备列表 -->
        <section class="page" data-page="devices">
            <div class="page-head">
                <h1>📱 设备管理</h1>
                <p class="page-desc">管理已注册设备：查看在线状态、撤销授权或彻底删除</p>
            </div>
            <div class="card">
                <div class="card-header">
                    <div class="card-title">已注册设备列表</div>
                </div>
                <table>
                    <thead>
                        <tr>
                            <th>设备 ID</th>
                            <th>在线状态</th>
                            <th>授权状态</th>
                            <th>凭证版本</th>
                            <th>注册时间</th>
                            <th>管理操作</th>
                        </tr>
                    </thead>
                    <tbody id="devicesBody">${devicesRows}</tbody>
                </table>
            </div>
            <div class="card">
                <div class="card-header">
                    <div class="card-title">📈 设备心跳监控 (24h)</div>
                    <div id="monitorUpdatedAt" class="badge" style="background: rgba(148,163,184,0.12); color: var(--text-muted);">等待数据...</div>
                </div>
                <div class="monitor-stats">
                    <div class="stat-card">
                        <div class="stat-icon">🟢</div>
                        <div class="stat-label">当前在线</div>
                        <div class="stat-value" id="monOnline">-</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-icon">📶</div>
                        <div class="stat-label">24h 在线率</div>
                        <div class="stat-value" id="monUptime">-</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-icon">⚠️</div>
                        <div class="stat-label">24h 掉线次数</div>
                        <div class="stat-value" id="monOffline">-</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-icon">📱</div>
                        <div class="stat-label">设备总数</div>
                        <div class="stat-value" id="monTotal">-</div>
                    </div>
                </div>
                <div class="monitor-chart">
                    <canvas id="onlineRateChart" height="160"></canvas>
                    <div class="chart-hint">舰队 24h 在线率（按小时）· 悬停查看具体数值</div>
                </div>
                <div class="monitor-body">
                    <div class="monitor-col">
                        <div class="monitor-subtitle">设备明细（24h 在线时长占比）</div>
                        <div id="monitorDeviceRows"><div class="empty-state" style="padding:1rem;">加载中...</div></div>
                    </div>
                    <div class="monitor-col">
                        <div class="monitor-subtitle">最近心跳事件</div>
                        <div id="monitorEventList"><div class="empty-state" style="padding:1rem;">加载中...</div></div>
                    </div>
                </div>
            </div>
        </section>

        <!-- 🔔 通知推送 (Hook Notify) -->
        <section class="page" data-page="notify">
            <div class="page-head">
                <h1>🔔 通知推送</h1>
                <p class="page-desc">向设备推送通知内容（支持中文，显示在设备屏幕），并查看 Hook 推送日志</p>
            </div>
            <div class="card">
                <div class="card-header">
                    <div class="card-title">🚀 在线发送通知 (Hook Notify)</div>
                </div>
                <div class="push-form">
                    <div class="push-grid">
                        <div class="form-group">
                            <label class="form-label">目标设备</label>
                            <select id="hookTargetDevice" class="form-select">${deviceOptions}</select>
                        </div>
                        <div class="form-group">
                            <label class="form-label">标题</label>
                            <input id="hookTitle" type="text" class="form-input" maxlength="32" value="Agent done">
                        </div>
                    </div>
                    <div class="form-group">
                        <label class="form-label">内容</label>
                        <textarea id="hookContent" class="form-input" rows="3" maxlength="2000" placeholder="要推送到设备屏幕显示的通知内容"></textarea>
                    </div>
                    <button type="button" id="hookSendBtn" class="btn btn-primary" style="padding:0.75rem;font-size:1rem;" onclick="pushHookNotify()">
                        🔔 立即发送到设备
                    </button>
                    <div id="hookStatus" style="display:none;padding:0.75rem;border-radius:6px;font-size:0.85rem;"></div>
                </div>
            </div>
            <div class="card">
                <div class="card-header">
                    <div class="card-title">🔔 Hook 推送日志 (最近 ${hookLogs.length} 条)</div>
                </div>
                <table>
                    <thead>
                        <tr>
                            <th>设备 ID</th>
                            <th>推送时间</th>
                            <th>标题</th>
                            <th>内容</th>
                            <th>设备在线</th>
                            <th>结果</th>
                        </tr>
                    </thead>
                    <tbody id="hookLogBody">${hookLogsRows}</tbody>
                </table>
            </div>
        </section>

        <script>
        // ----------------- 远程屏幕快照与截屏 (Remote Snapshot Studio) -----------------
        let castWs = null;
        let castAutoTimer = null;
        let castCurrentSeq = 0;
        let castStarted = false;

        const castCanvas = document.getElementById("castCanvas");
        const castCtx = castCanvas.getContext("2d");
        const castPlaceholder = document.getElementById("castPlaceholder");
        const castStatusBadge = document.getElementById("castStatusBadge");
        const castStatusDot = document.getElementById("castStatusDot");
        const castStatusText = document.getElementById("castStatusText");
        const castSliceCount = document.getElementById("castSliceCount");
        const castSeq = document.getElementById("castSeq");
        const castTime = document.getElementById("castTime");
        const castTargetDevice = document.getElementById("castTargetDevice");
        const castAlert = document.getElementById("castAlert");
        const autoSnapshotCheck = document.getElementById("autoSnapshotCheck");

        castCtx.fillStyle = "#000000";
        castCtx.fillRect(0, 0, 240, 320);

        function updateCastStatus(status, text) {
            if (status === "connected") {
                castStatusBadge.style.background = "rgba(46, 160, 67, 0.15)";
                castStatusBadge.style.color = "#3fb950";
                castStatusDot.className = "dot dot-online";
                castStatusText.innerText = text || "已就绪";
            } else if (status === "receiving") {
                castStatusBadge.style.background = "rgba(31, 111, 235, 0.15)";
                castStatusBadge.style.color = "#58a6ff";
                castStatusDot.className = "dot dot-online";
                castStatusText.innerText = text || "正在传输快照...";
            } else {
                castStatusBadge.style.background = "rgba(110, 118, 129, 0.2)";
                castStatusBadge.style.color = "var(--text-muted)";
                castStatusDot.className = "dot dot-offline";
                castStatusText.innerText = text || "未连接";
            }
        }

        function initCastWebSocket() {
            if (castWs) {
                try { castWs.close(); } catch {}
                castWs = null;
            }
            const deviceId = castTargetDevice.value;
            if (!deviceId) return;

            const proto = location.protocol === "https:" ? "wss:" : "ws:";
            const wsUrl = proto + "//" + location.host + "/admin/screencast/ws?deviceId=" + encodeURIComponent(deviceId);
            
            try {
                castWs = new WebSocket(wsUrl);
            } catch (e) {
                updateCastStatus("disconnected", "连接异常");
                return;
            }

            castWs.onopen = () => {
                updateCastStatus("connected", "已就绪");
            };

            castWs.onmessage = (event) => {
                try {
                    const msg = JSON.parse(event.data);
                    if (msg.type === "screencast" && typeof msg.slice === "number" && msg.data) {
                        renderSlice(msg);
                    }
                } catch (err) {
                    console.error("Parse cast message error:", err);
                }
            };

            castWs.onclose = () => {
                updateCastStatus("disconnected", "已断开");
            };

            castWs.onerror = () => {
                updateCastStatus("disconnected", "连接错误");
            };
        }

        function renderSlice(msg) {
            castPlaceholder.style.display = "none";
            updateCastStatus("receiving", "同步画面中...");

            const binaryString = atob(msg.data);
            const len = binaryString.length;
            const bytes = new Uint8Array(len);
            for (let i = 0; i < len; i++) {
                bytes[i] = binaryString.charCodeAt(i);
            }

            const lines = msg.lines || 5;
            const width = msg.w || 240;
            const y = msg.y !== undefined ? msg.y : (msg.slice * lines);
            const pixelCount = width * lines;
            const imgData = castCtx.createImageData(width, lines);
            const data = imgData.data;

            for (let i = 0; i < pixelCount; i++) {
                const low = bytes[i * 2];
                const high = bytes[i * 2 + 1];
                const rgb565 = (high << 8) | low;

                const r = ((rgb565 >> 11) & 0x1F) * 255 / 31;
                const g = ((rgb565 >> 5) & 0x3F) * 255 / 63;
                const b = (rgb565 & 0x1F) * 255 / 31;

                const p = i * 4;
                data[p] = r;
                data[p + 1] = g;
                data[p + 2] = b;
                data[p + 3] = 255;
            }

            castCtx.putImageData(imgData, 0, y);

            if (msg.seq !== castCurrentSeq) {
                castCurrentSeq = msg.seq;
                castSeq.innerText = "帧 #" + msg.seq;
            }
            const total = msg.total || 64;
            castSliceCount.innerText = (msg.slice + 1) + "/" + total + " 片";

            if (msg.slice + 1 >= total) {
                const timeStr = new Date().toLocaleTimeString();
                castTime.innerText = timeStr;
                updateCastStatus("connected", "快照就绪 (" + timeStr + ")");
            }
        }

        async function requestCapture() {
            const deviceId = castTargetDevice.value;
            if (!deviceId) {
                alert("请先选择设备！");
                return;
            }
            if (!castWs || castWs.readyState !== WebSocket.OPEN) {
                initCastWebSocket();
            }
            updateCastStatus("receiving", "请求截屏中...");
            await sendScreencastCommand(deviceId, "capture");
        }

        function toggleAutoSnapshot() {
            if (autoSnapshotCheck.checked) {
                requestCapture();
                castAutoTimer = setInterval(() => {
                    requestCapture();
                }, 5000);
            } else {
                if (castAutoTimer) {
                    clearInterval(castAutoTimer);
                    castAutoTimer = null;
                }
            }
        }

        async function sendScreencastCommand(deviceId, action) {
            try {
                const res = await fetch("/admin/screencast/command", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ deviceId, action })
                });
                const result = await res.json();
                if (!result.sent) {
                    castAlert.style.display = "block";
                    castAlert.style.background = "rgba(235, 179, 56, 0.15)";
                    castAlert.style.color = "#d29922";
                    castAlert.innerText = "⚠️ 设备可能处于离线状态，指令已尝试下发。";
                } else {
                    castAlert.style.display = "none";
                }
            } catch (err) {
                console.error("Send cast command error:", err);
            }
        }

        function downloadScreenshot() {
            const link = document.createElement("a");
            link.download = "passport_screenshot_" + Date.now() + ".png";
            link.href = castCanvas.toDataURL("image/png");
            link.click();
        }

        function reconnectCastWs() {
            initCastWebSocket();
        }

        castTargetDevice.addEventListener("change", () => {
            initCastWebSocket();
        });

        // ---------------- 概览统计数字滚动动画 ----------------
        let statsAnimated = false;
        function animateStats() {
            document.querySelectorAll('[data-page="overview"] .stat-value').forEach((el) => {
                const target = parseInt(el.textContent.replace(/[^0-9]/g, ""), 10);
                if (Number.isNaN(target)) return;
                const duration = 800;
                const start = performance.now();
                function tick(now) {
                    const p = Math.min((now - start) / duration, 1);
                    const eased = 1 - Math.pow(1 - p, 3);
                    el.textContent = Math.round(target * eased);
                    if (p < 1) requestAnimationFrame(tick);
                }
                requestAnimationFrame(tick);
            });
        }

        // ----------------- 顶部导航 Tab 分区 (Top Navigation Tabs) -----------------
        function switchTab(name) {
            document.querySelectorAll(".nav-tab").forEach((tab) => {
                tab.classList.toggle("active", tab.dataset.tab === name);
            });
            document.querySelectorAll(".page").forEach((page) => {
                const isActive = page.dataset.page === name;
                page.classList.toggle("active", isActive);
                // 首次进入快照页时才建立 WebSocket，避免后台空连接
                if (isActive && name === "snapshot" && !castStarted) {
                    castStarted = true;
                    initCastWebSocket();
                }
                // 概览页首次展示时滚动统计数字
                if (isActive && name === "overview" && !statsAnimated) {
                    statsAnimated = true;
                    animateStats();
                }
                // 设备页首次展示时加载心跳监控
                if (isActive && name === "devices" && !monitoringLoaded) {
                    monitoringLoaded = true;
                    fetchMonitoring();
                }
                // 信息壁纸页首次展示时生成预览
                if (isActive && name === "wallpaper" && !wallpaperLoaded) {
                    wallpaperLoaded = true;
                    refreshWallpaper();
                }
            });
            try { history.replaceState(null, "", "#" + name); } catch {}
        }

        document.querySelectorAll(".nav-tab").forEach((tab) => {
            tab.addEventListener("click", () => switchTab(tab.dataset.tab));
        });

        const initialTab = (location.hash || "").slice(1);
        switchTab(["snapshot", "images", "wallpaper", "requests", "notify", "devices"].includes(initialTab) ? initialTab : "overview");

        // ---------------- 后台数据自动刷新 (15s 轮询) ----------------
        const DASHBOARD_POLL_INTERVAL_MS = 15000;
        let dashboardPollTimer = null;

        async function pollDashboardData() {
            if (document.hidden) return;
            try {
                const res = await fetch("/admin/dashboard-data", { headers: { "Accept": "application/json" } });
                if (res.status === 401) {
                    // 登录失效后停止轮询，避免无意义请求
                    if (dashboardPollTimer) { clearInterval(dashboardPollTimer); dashboardPollTimer = null; }
                    return;
                }
                if (!res.ok) return;
                const data = await res.json();
                const stats = data.stats || {};
                const setStat = (id, v) => {
                    const el = document.getElementById(id);
                    if (el && typeof v === "number") el.textContent = String(v);
                };
                setStat("statTotal", stats.total);
                setStat("statOnline", stats.online);
                setStat("statActive", stats.active);
                setStat("statPending", stats.pending);
                if (data.deviceRows) {
                    const body = document.getElementById("devicesBody");
                    if (body && body.innerHTML !== data.deviceRows) body.innerHTML = data.deviceRows;
                }
                if (data.pushLogRows) {
                    const body = document.getElementById("pushLogBody");
                    if (body && body.innerHTML !== data.pushLogRows) body.innerHTML = data.pushLogRows;
                }
                if (data.hookLogRows) {
                    const body = document.getElementById("hookLogBody");
                    if (body && body.innerHTML !== data.hookLogRows) body.innerHTML = data.hookLogRows;
                }
                // 设备页激活时同步刷新心跳监控
                const activePage = document.querySelector(".page.active");
                if (activePage && activePage.dataset.page === "devices") {
                    fetchMonitoring();
                }
            } catch {}
        }

        dashboardPollTimer = setInterval(pollDashboardData, DASHBOARD_POLL_INTERVAL_MS);
        // 首屏渲染后 3 秒先拉一次，与页面初始数据对齐
        setTimeout(pollDashboardData, 3000);

        // ---------------- 设备心跳监控 (Heartbeat Monitoring) ----------------
        let monitoringLoaded = false;
        let monitoringBusy = false;
        let monChartData = null;

        function fmtAgo(ts, now) {
            if (!ts) return "从未";
            const diff = Math.max(0, now - ts);
            if (diff < 60) return "刚刚";
            if (diff < 3600) return Math.floor(diff / 60) + " 分钟前";
            if (diff < 86400) return Math.floor(diff / 3600) + " 小时前";
            return Math.floor(diff / 86400) + " 天前";
        }

        async function fetchMonitoring() {
            if (monitoringBusy) return;
            monitoringBusy = true;
            try {
                const res = await fetch("/admin/monitoring", { headers: { "Accept": "application/json" } });
                if (!res.ok) return;
                const data = await res.json();
                renderMonitoring(data);
            } catch {} finally {
                monitoringBusy = false;
            }
        }

        function renderMonitoring(data) {
            const now = data.now;
            const setText = (id, v) => {
                const el = document.getElementById(id);
                if (el) el.textContent = String(v);
            };
            setText("monOnline", data.fleet.online_now);
            setText("monUptime", data.fleet.uptime_pct_24h + "%");
            setText("monOffline", data.fleet.offline_events_24h);
            setText("monTotal", data.fleet.total);
            const updated = document.getElementById("monitorUpdatedAt");
            if (updated) updated.textContent = "更新于 " + new Date(now * 1000).toLocaleTimeString();

            const devBox = document.getElementById("monitorDeviceRows");
            if (devBox) {
                let html = "";
                for (const d of data.devices) {
                    const dot = d.online_now
                        ? '<span class="dot dot-online"></span>'
                        : '<span class="dot dot-offline"></span>';
                    html += '<div class="mon-device-row">' + dot +
                        '<span class="code-mono" style="font-size:0.78rem;">' + d.device_id + '</span>' +
                        '<div class="mon-uptime-bar"><div class="mon-uptime-fill" style="width:' + d.uptime_pct_24h + '%"></div></div>' +
                        '<span class="mon-pct">' + d.uptime_pct_24h + '%</span>' +
                        '<span style="color:var(--text-muted);font-size:0.72rem;white-space:nowrap;">' + fmtAgo(d.last_seen, now) + '</span>' +
                        '</div>';
                }
                if (!data.devices.length) html = '<div class="empty-state" style="padding:1rem;">暂无设备</div>';
                devBox.innerHTML = html;
            }

            const evBox = document.getElementById("monitorEventList");
            if (evBox) {
                let html = "";
                for (const e of data.recent_events) {
                    const on = e.event === "online";
                    html += '<div class="mon-event-row">' +
                        '<span class="mon-event-badge ' + (on ? "mon-event-online" : "mon-event-offline") + '">' + (on ? "▲ 上线" : "▼ 离线") + '</span>' +
                        '<span class="code-mono" style="font-size:0.72rem;">' + e.device_id + '</span>' +
                        '<span style="color:var(--text-muted);font-size:0.72rem;margin-left:auto;white-space:nowrap;">' + new Date(e.created_at * 1000).toLocaleTimeString() + '</span>' +
                        '</div>';
                }
                if (!data.recent_events.length) html = '<div class="empty-state" style="padding:1rem;">暂无事件（设备连接后自动记录）</div>';
                evBox.innerHTML = html;
            }

            drawOnlineRateChart(data.hourly || [], now);
        }

        function drawOnlineRateChart(hourly, now, hoverIdx) {
            const canvas = document.getElementById("onlineRateChart");
            if (!canvas) return;
            monChartData = { hourly: hourly, now: now };
            const dpr = window.devicePixelRatio || 1;
            const cssW = Math.max(canvas.clientWidth || 900, 320);
            const cssH = 160;
            canvas.width = Math.round(cssW * dpr);
            canvas.height = Math.round(cssH * dpr);
            const ctx = canvas.getContext("2d");
            ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
            ctx.clearRect(0, 0, cssW, cssH);
            const padL = 30, padR = 10, padT = 12, padB = 20;
            const w = cssW - padL - padR, h = cssH - padT - padB;
            const x = (i) => padL + (i / 23) * w;
            const y = (v) => padT + h - (v / 100) * h;

            ctx.font = "10px ui-monospace, SFMono-Regular, Consolas, monospace";
            ctx.lineWidth = 1;
            for (const gv of [0, 25, 50, 75, 100]) {
                ctx.strokeStyle = "rgba(148,163,184,0.12)";
                ctx.beginPath(); ctx.moveTo(padL, y(gv)); ctx.lineTo(cssW - padR, y(gv)); ctx.stroke();
                ctx.fillStyle = "rgba(148,163,184,0.65)";
                ctx.fillText(gv + "%", 4, y(gv) + 3);
            }

            ctx.beginPath();
            ctx.moveTo(x(0), y(hourly[0] || 0));
            for (let i = 1; i < 24; i++) ctx.lineTo(x(i), y(hourly[i] || 0));
            ctx.lineTo(x(23), padT + h); ctx.lineTo(x(0), padT + h); ctx.closePath();
            const grad = ctx.createLinearGradient(0, padT, 0, padT + h);
            grad.addColorStop(0, "rgba(99,102,241,0.4)");
            grad.addColorStop(1, "rgba(99,102,241,0.02)");
            ctx.fillStyle = grad; ctx.fill();

            ctx.beginPath();
            ctx.moveTo(x(0), y(hourly[0] || 0));
            for (let i = 1; i < 24; i++) ctx.lineTo(x(i), y(hourly[i] || 0));
            ctx.strokeStyle = "#818cf8";
            ctx.lineWidth = 2;
            ctx.stroke();

            for (let i = 0; i < 24; i += 6) {
                const t = new Date((now - (23 - i) * 3600) * 1000);
                const label = String(t.getHours()).padStart(2, "0") + ":00";
                ctx.fillStyle = "rgba(148,163,184,0.7)";
                ctx.fillText(label, x(i) - 14, cssH - 6);
            }

            if (typeof hoverIdx === "number") {
                ctx.strokeStyle = "rgba(129,140,248,0.6)";
                ctx.setLineDash([4, 4]);
                ctx.beginPath(); ctx.moveTo(x(hoverIdx), padT); ctx.lineTo(x(hoverIdx), padT + h); ctx.stroke();
                ctx.setLineDash([]);
                const val = String(hourly[hoverIdx] || 0) + "%";
                ctx.font = "10px ui-monospace, SFMono-Regular, Consolas, monospace";
                const tw = ctx.measureText(val).width + 10;
                const tx = Math.min(Math.max(x(hoverIdx) - tw / 2, 2), cssW - tw - 2);
                ctx.fillStyle = "rgba(8,10,16,0.92)";
                ctx.fillRect(tx, 2, tw, 16);
                ctx.fillStyle = "#c7d2fe";
                ctx.fillText(val, tx + 5, 14);
            }
        }

        const monChartCanvas = document.getElementById("onlineRateChart");
        if (monChartCanvas) {
            monChartCanvas.addEventListener("mousemove", (e) => {
                if (!monChartData) return;
                const rect = monChartCanvas.getBoundingClientRect();
                const padL = 30, padR = 10;
                const w = (monChartCanvas.clientWidth || rect.width) - padL - padR;
                const i = Math.round(((e.clientX - rect.left - padL) / w) * 23);
                if (i < 0 || i > 23) { monChartCanvas.style.cursor = "default"; return; }
                monChartCanvas.style.cursor = "crosshair";
                drawOnlineRateChart(monChartData.hourly, monChartData.now, i);
            });
            monChartCanvas.addEventListener("mouseleave", () => {
                if (monChartData) drawOnlineRateChart(monChartData.hourly, monChartData.now);
            });
            window.addEventListener("resize", () => {
                if (monChartData) drawOnlineRateChart(monChartData.hourly, monChartData.now);
            });
        }

        // ---------------- 信息壁纸 (Info Wallpaper) ----------------
        let wallpaperLoaded = false;

        function wallpaperStatus(kind, text) {
            const box = document.getElementById("wallpaperStatus");
            if (!box) return;
            box.style.display = "block";
            if (kind === "success") {
                box.style.background = "rgba(46, 160, 67, 0.15)";
                box.style.border = "1px solid rgba(46, 160, 67, 0.3)";
                box.style.color = "#3fb950";
            } else if (kind === "error") {
                box.style.background = "rgba(248, 81, 73, 0.15)";
                box.style.border = "1px solid rgba(248, 81, 73, 0.3)";
                box.style.color = "#f85149";
            } else {
                box.style.background = "rgba(56, 139, 253, 0.1)";
                box.style.border = "1px solid rgba(56, 139, 253, 0.3)";
                box.style.color = "#58a6ff";
            }
            box.innerText = text;
        }

        async function refreshWallpaper() {
            try {
                const res = await fetch("/admin/wallpaper/preview", { headers: { "Accept": "application/json" } });
                const data = await res.json();
                if (!data.ok) throw new Error(data.error || "preview failed");
                const img = document.getElementById("wallpaperImg");
                if (img) img.src = data.dataUrl;
                const notesBox = document.getElementById("wallpaperNotes");
                if (notesBox && !notesBox.value && data.notes) notesBox.value = data.notes.join("\\n");
            } catch (err) {
                wallpaperStatus("error", "预览失败: " + err);
            }
        }

        async function pushWallpaper() {
            const deviceId = document.getElementById("wallpaperDevice").value;
            try {
                const res = await fetch("/admin/wallpaper/push", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ device_id: deviceId || undefined })
                });
                const data = await res.json();
                if (data.ok) {
                    wallpaperStatus("success", "✅ 已推送 " + data.pushed.length + " 台设备" + (deviceId ? "" : "（全部在线设备）"));
                } else {
                    wallpaperStatus("error", "❌ 推送失败: " + (data.error || "未知错误"));
                }
            } catch (err) {
                wallpaperStatus("error", "❌ 推送失败: " + err);
            }
        }

        async function saveWallpaperNotes() {
            const text = document.getElementById("wallpaperNotes").value;
            try {
                const res = await fetch("/admin/wallpaper/notes", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ text: text })
                });
                const data = await res.json();
                if (data.ok) {
                    wallpaperStatus("success", "✅ 备注已保存，点「刷新预览」即可看到效果");
                } else {
                    wallpaperStatus("error", "❌ 保存失败");
                }
            } catch (err) {
                wallpaperStatus("error", "❌ 保存失败: " + err);
            }
        }

        // ----------------- 图片推送工作台 (Image Studio) -----------------
        let currentBase64 = "";

        const filePicker = document.getElementById("filePicker");
        const canvas = document.getElementById("previewCanvas");
        const ctx = canvas.getContext("2d");
        const placeholder = document.getElementById("placeholder");
        const sizeBadge = document.getElementById("sizeBadge");

        filePicker.addEventListener("change", function(e) {
            const file = e.target.files[0];
            if (!file) return;
            const reader = new FileReader();
            reader.onload = function(event) {
                const img = new Image();
                img.onload = function() {
                    // 居中等比例缩放并裁剪为 240 x 320
                    const targetW = 240;
                    const targetH = 320;
                    const targetRatio = targetW / targetH;
                    const imgRatio = img.width / img.height;

                    let sw, sh, sx, sy;
                    if (imgRatio > targetRatio) {
                        sh = img.height;
                        sw = img.height * targetRatio;
                        sx = (img.width - sw) / 2;
                        sy = 0;
                    } else {
                        sw = img.width;
                        sh = img.width / targetRatio;
                        sx = 0;
                        sy = (img.height - sh) / 2;
                    }

                    canvas.width = targetW;
                    canvas.height = targetH;
                    ctx.drawImage(img, sx, sy, sw, sh, 0, 0, targetW, targetH);

                    // 转为高质量 JPEG
                    currentBase64 = canvas.toDataURL("image/jpeg", 0.85);
                    placeholder.style.display = "none";
                    canvas.style.display = "block";

                    const rawLength = currentBase64.length * 3 / 4;
                    sizeBadge.innerText = "压缩大小: " + (rawLength / 1024).toFixed(1) + " KB (240×320 JPEG)";
                };
                img.src = event.target.result;
            };
            reader.readAsDataURL(file);
        });

        async function pushCurrentCanvas() {
            if (!currentBase64) {
                alert("请先选择一张图片！");
                return;
            }
            const deviceId = document.getElementById("targetDevice").value;
            if (!deviceId) {
                alert("请选择目标设备！");
                return;
            }
            const title = document.getElementById("imageTitle").value || "Image";
            await sendPushRequest(deviceId, title, currentBase64);
        }

        async function repushImage(deviceId, title, rawBase64) {
            await sendPushRequest(deviceId, title, rawBase64);
        }

        async function sendPushRequest(deviceId, title, imageData) {
            const btn = document.getElementById("pushBtn");
            const alertBox = document.getElementById("statusAlert");
            btn.disabled = true;
            btn.innerText = "正在推送中...";
            alertBox.style.display = "none";

            try {
                const res = await fetch("/admin/devices/images/push", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ device_id: deviceId, title, image_data: imageData })
                });
                const result = await res.json();
                alertBox.style.display = "block";
                if (result.ok) {
                    if (result.online && result.sent) {
                        alertBox.style.background = "rgba(46, 160, 67, 0.15)";
                        alertBox.style.border = "1px solid rgba(46, 160, 67, 0.3)";
                        alertBox.style.color = "#3fb950";
                        alertBox.innerHTML = "✅ <strong>推送成功！</strong> 图片已通过 WebSocket 实时推送到设备屏幕并存入历史。";
                    } else {
                        alertBox.style.background = "rgba(235, 179, 56, 0.15)";
                        alertBox.style.border = "1px solid rgba(235, 179, 56, 0.3)";
                        alertBox.style.color = "#d29922";
                        alertBox.innerHTML = "💾 <strong>图片已保存！</strong> 设备当前处于离线态，开机连接后可查看。";
                    }
                } else {
                    alertBox.style.background = "rgba(248, 81, 73, 0.15)";
                    alertBox.style.border = "1px solid rgba(248, 81, 73, 0.3)";
                    alertBox.style.color = "#f85149";
                    alertBox.innerHTML = "❌ 推送失败: " + (result.error || "未知错误");
                }
            } catch (err) {
                alert("网络请求失败: " + err);
            } finally {
                btn.disabled = false;
                btn.innerText = "🚀 立即推送到设备屏幕";
            }
        }

        // ---------------- 在线发送 Hook（测试推送） ----------------
        async function pushHookNotify() {
            const deviceId = document.getElementById("hookTargetDevice").value;
            const title = document.getElementById("hookTitle").value || "Agent done";
            const content = document.getElementById("hookContent").value.trim();
            const btn = document.getElementById("hookSendBtn");
            const statusBox = document.getElementById("hookStatus");

            if (!deviceId) { alert("请先选择目标设备！"); return; }
            if (!content) { alert("请输入要推送的内容！"); return; }

            btn.disabled = true;
            btn.innerText = "正在发送...";
            statusBox.style.display = "none";
            try {
                const res = await fetch("/admin/hook/push", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ device_id: deviceId, title, content })
                });
                const result = await res.json();
                statusBox.style.display = "block";
                if (result.ok) {
                    if (result.sent) {
                        statusBox.style.background = "rgba(46, 160, 67, 0.15)";
                        statusBox.style.border = "1px solid rgba(46, 160, 67, 0.3)";
                        statusBox.style.color = "#3fb950";
                        statusBox.innerHTML = "✅ <strong>已发送到设备</strong>（设备在线）。请到设备「Kiro Passport」页面查看。";
                    } else if (result.online) {
                        statusBox.style.background = "rgba(235, 179, 56, 0.15)";
                        statusBox.style.border = "1px solid rgba(235, 179, 56, 0.3)";
                        statusBox.style.color = "#d29922";
                        statusBox.innerHTML = "⚠️ 设备在线但未送达（可能页面未就绪）。";
                    } else {
                        statusBox.style.background = "rgba(235, 179, 56, 0.15)";
                        statusBox.style.border = "1px solid rgba(235, 179, 56, 0.3)";
                        statusBox.style.color = "#d29922";
                        statusBox.innerHTML = "⚠️ 设备当前处于<strong>离线</strong>状态。已记录到 Hook 日志，设备上线后可重发。";
                    }
                } else {
                    statusBox.style.background = "rgba(248, 81, 73, 0.15)";
                    statusBox.style.border = "1px solid rgba(248, 81, 73, 0.3)";
                    statusBox.style.color = "#f85149";
                    statusBox.innerHTML = "❌ 发送失败: " + (result.error || "未知错误");
                }
            } catch (err) {
                statusBox.style.display = "block";
                statusBox.style.background = "rgba(248, 81, 73, 0.15)";
                statusBox.style.border = "1px solid rgba(248, 81, 73, 0.3)";
                statusBox.style.color = "#f85149";
                statusBox.innerHTML = "❌ 网络请求失败: " + err;
            } finally {
                btn.disabled = false;
                btn.innerText = "🔔 立即发送到设备";
            }
        }

        async function deleteImage(imageId) {
            if (!confirm("确定要删除此图片吗？")) return;
            try {
                const res = await fetch("/admin/devices/images/delete", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ image_id: imageId })
                });
                const result = await res.json();
                if (result.ok) {
                    location.reload();
                } else {
                    alert("删除失败: " + result.error);
                }
            } catch (err) {
                alert("网络请求失败: " + err);
            }
        }

        // ----------------- 审批推送 (Approval Push Center) -----------------
        const pushResultBox = document.getElementById("pushResult");

        function showPushResult(kind, text) {
            pushResultBox.style.display = "block";
            if (kind === "success") {
                pushResultBox.style.background = "rgba(46, 160, 67, 0.15)";
                pushResultBox.style.border = "1px solid rgba(46, 160, 67, 0.3)";
                pushResultBox.style.color = "#3fb950";
            } else if (kind === "error") {
                pushResultBox.style.background = "rgba(248, 81, 73, 0.15)";
                pushResultBox.style.border = "1px solid rgba(248, 81, 73, 0.3)";
                pushResultBox.style.color = "#f85149";
            } else {
                pushResultBox.style.background = "rgba(56, 139, 253, 0.1)";
                pushResultBox.style.border = "1px solid rgba(56, 139, 253, 0.3)";
                pushResultBox.style.color = "#58a6ff";
            }
            pushResultBox.innerText = text;
        }

        async function sendAdminPush() {
            const deviceId = document.getElementById("pushDevice").value;
            const tool = document.getElementById("pushTool").value.trim();
            const summary = document.getElementById("pushSummary").value.trim();
            const ttl = parseInt(document.getElementById("pushTtl").value, 10);
            const btn = document.getElementById("pushSendBtn");
            if (!deviceId) {
                alert("请先选择目标设备！");
                return;
            }
            if (!/^[A-Za-z0-9._:-]{1,31}$/.test(tool)) {
                alert("工具名称仅允许字母、数字、._:-，最长 31 字符");
                return;
            }
            if (!summary) {
                alert("请填写推送内容 (summary)！");
                return;
            }
            if (summary.length > 71 || /["\\\\]/.test(summary)) {
                alert("推送内容最长 71 字符，且不能包含引号或反斜杠");
                return;
            }

            btn.disabled = true;
            btn.innerText = "正在推送...";
            pushResultBox.style.display = "none";
            try {
                const res = await fetch("/admin/push", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ device_id: deviceId, tool: tool, summary: summary, ttl_seconds: ttl })
                });
                const result = await res.json();
                if (!res.ok || !result.ok) {
                    showPushResult("error", "❌ 推送失败: " + (result.error || ("HTTP " + res.status)));
                    return;
                }
                let status = result.status;
                let reason = result.reason || "";
                if (status === "pending") {
                    showPushResult("info", "⏳ 请求已推送到设备，等待用户按键决定...");
                    for (let i = 0; i < 12; i++) {
                        await new Promise((resolve) => setTimeout(resolve, 2000));
                        try {
                            const sr = await fetch("/admin/push/status?request_id=" + encodeURIComponent(result.request_id));
                            if (sr.ok) {
                                const s = await sr.json();
                                if (s.status !== "pending") {
                                    status = s.status;
                                    reason = s.reason || "";
                                    break;
                                }
                            }
                        } catch {}
                    }
                }
                const reasonText = reason ? (" (" + reason + ")") : "";
                if (status === "allow") {
                    showPushResult("success", "✅ 设备已批准该请求" + reasonText);
                } else if (status === "deny") {
                    showPushResult("error", "❌ 请求已拒绝 / 未送达" + reasonText);
                } else {
                    showPushResult("info", "⏳ 仍在等待设备决定，可稍后刷新页面查看日志结果。");
                }
            } catch (err) {
                showPushResult("error", "❌ 网络请求失败: " + err);
            } finally {
                btn.disabled = false;
                btn.innerText = "🚀 立即推送审批请求";
            }
        }
        // ----------------- 通知推送 (Hook Notify) -----------------
        async function pushHookNotify() {
            const deviceId = document.getElementById("hookTargetDevice").value;
            const title = document.getElementById("hookTitle").value || "Agent done";
            const content = document.getElementById("hookContent").value.trim();
            const btn = document.getElementById("hookSendBtn");
            const statusBox = document.getElementById("hookStatus");

            if (!deviceId) { alert("请先选择目标设备！"); return; }
            if (!content) { alert("请输入要推送的内容！"); return; }

            btn.disabled = true;
            btn.innerText = "正在发送...";
            statusBox.style.display = "none";
            try {
                const res = await fetch("/admin/hook/push", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ device_id: deviceId, title, content })
                });
                const result = await res.json();
                statusBox.style.display = "block";
                if (result.ok) {
                    if (result.sent) {
                        statusBox.style.background = "rgba(46, 160, 67, 0.15)";
                        statusBox.style.border = "1px solid rgba(46, 160, 67, 0.3)";
                        statusBox.style.color = "#3fb950";
                        statusBox.innerHTML = "✅ <strong>已发送到设备</strong>（设备在线）。请到设备「Kiro Passport」页面查看。";
                    } else if (result.online) {
                        statusBox.style.background = "rgba(235, 179, 56, 0.15)";
                        statusBox.style.border = "1px solid rgba(235, 179, 56, 0.3)";
                        statusBox.style.color = "#d29922";
                        statusBox.innerHTML = "⚠️ 设备在线但未送达（可能页面未就绪）。";
                    } else {
                        statusBox.style.background = "rgba(235, 179, 56, 0.15)";
                        statusBox.style.border = "1px solid rgba(235, 179, 56, 0.3)";
                        statusBox.style.color = "#d29922";
                        statusBox.innerHTML = "⚠️ 设备当前处于<strong>离线</strong>状态。已记录到 Hook 日志，设备上线后可重发。";
                    }
                } else {
                    statusBox.style.background = "rgba(248, 81, 73, 0.15)";
                    statusBox.style.border = "1px solid rgba(248, 81, 73, 0.3)";
                    statusBox.style.color = "#f85149";
                    statusBox.innerHTML = "❌ 发送失败: " + (result.error || "未知错误");
                }
            } catch (err) {
                statusBox.style.display = "block";
                statusBox.style.background = "rgba(248, 81, 73, 0.15)";
                statusBox.style.border = "1px solid rgba(248, 81, 73, 0.3)";
                statusBox.style.color = "#f85149";
                statusBox.innerHTML = "❌ 网络请求失败: " + err;
            } finally {
                btn.disabled = false;
                btn.innerText = "🔔 立即发送到设备";
            }
        }
        </script>
    `;

    return htmlPage("管理控制台", content, 200, undefined, renderNav("overview"));
}

/** 存储图片到历史并尝试实时推送到设备（在线则经 WebSocket 下发）。 */
async function pushImageToDevice(
    env: Env,
    deviceId: string,
    title: string,
    rawBase64: string,
): Promise<{ ok: boolean; image_id: string; sent: boolean; online: boolean; error?: string }> {
    let clean = rawBase64;
    if (clean.includes(",")) clean = clean.split(",")[1];
    const imageId = crypto.randomUUID();
    const now = nowSeconds();
    try {
        await env.DB.prepare(
            "INSERT INTO device_images (image_id, device_id, title, image_data, created_at) VALUES (?1, ?2, ?3, ?4, ?5)",
        ).bind(imageId, deviceId, title.slice(0, 64), clean, now).run();
    } catch (err) {
        console.error("Store image failed", err);
        return { ok: false, image_id: "", sent: false, online: false, error: "store_failed" };
    }
    let sent = false;
    let online = false;
    try {
        const relay = env.PASSPORTS.get(env.PASSPORTS.idFromName(deviceId));
        const res = await relay.fetch("https://passport.internal/internal/send-image", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ imageId, title, data: clean }),
        });
        if (res.ok) {
            const data = await res.json<{ sent?: boolean; online?: boolean }>();
            sent = Boolean(data?.sent);
            online = Boolean(data?.online);
        }
    } catch (err) {
        console.error("Relay send-image failed", err);
    }
    return { ok: true, image_id: imageId, sent, online };
}

async function adminPushImageWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const body = await request.json<{ device_id: string; title?: string; image_data: string }>().catch(() => null);
    if (!body || !isDeviceId(body.device_id) || typeof body.image_data !== "string" || !body.image_data) {
        return json({ error: "invalid image data" }, 400);
    }
    const result = await pushImageToDevice(env, body.device_id, body.title || "Image", body.image_data);
    if (!result.ok) return json({ error: result.error ?? "store failed" }, 500);
    return json({ ok: true, image_id: result.image_id, sent: result.sent, online: result.online });
}

/* ---------------- 信息壁纸 (Info Wallpaper) ---------------- */

const WALLPAPER_DEFAULT_CITY = "Shanghai";
const WALLPAPER_NOTES_KEY = "notes";

async function loadWallpaperNotes(env: Env): Promise<string[]> {
    try {
        const row = await env.DB.prepare("SELECT value FROM wallpaper_notes WHERE key = ?1")
            .bind(WALLPAPER_NOTES_KEY).first<{ value: string }>();
        return row ? row.value.split("\n").slice(0, 3) : [];
    } catch {
        return [];
    }
}

async function saveWallpaperNotes(env: Env, text: string): Promise<void> {
    const now = nowSeconds();
    await env.DB.prepare(
        "INSERT INTO wallpaper_notes (key, value, updated_at) VALUES (?1, ?2, ?3) " +
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at",
    ).bind(WALLPAPER_NOTES_KEY, text.slice(0, 300), now).run();
}

/** 拉取 Open-Meteo 当前天气（免费、无需 key）；失败返回 null，壁纸降级显示。 */
async function fetchCurrentWeather(env: Env): Promise<{ tempC: number; text: string } | null> {
    const lat = env.WALLPAPER_LAT || "31.2304";
    const lon = env.WALLPAPER_LON || "121.4737";
    try {
        const res = await fetch(
            `https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}&current=temperature_2m,weather_code&timezone=auto`,
            { headers: { "User-Agent": "kiro-passport-relay" } },
        );
        if (!res.ok) return null;
        const data = await res.json<{ current?: { temperature_2m?: number; weather_code?: number } }>();
        if (typeof data.current?.temperature_2m !== "number") return null;
        return { tempC: data.current.temperature_2m, text: weatherCodeLabel(data.current.weather_code ?? -1) };
    } catch (err) {
        console.error("Wallpaper weather fetch failed", err);
        return null;
    }
}

async function generateAndPushWallpaper(env: Env, deviceId?: string): Promise<{ pushed: string[] }> {
    const [notes, weather] = await Promise.all([loadWallpaperNotes(env), fetchCurrentWeather(env)]);
    const jpegBase64 = renderWallpaperJpeg({
        now: new Date(),
        city: env.WALLPAPER_CITY || WALLPAPER_DEFAULT_CITY,
        weather,
        notes,
    });
    let targets: string[];
    if (deviceId) {
        targets = [deviceId];
    } else {
        const rows = (await env.DB.prepare(
            "SELECT device_id FROM devices WHERE status = 'active'",
        ).all<{ device_id: string }>()).results;
        targets = rows.map((r) => r.device_id);
    }
    const pushed: string[] = [];
    for (const id of targets) {
        const result = await pushImageToDevice(env, id, "Info Wallpaper", jpegBase64);
        if (result.ok) pushed.push(id);
    }
    return { pushed };
}

async function adminWallpaperPreviewWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const [notes, weather] = await Promise.all([loadWallpaperNotes(env), fetchCurrentWeather(env)]);
    const jpegBase64 = renderWallpaperJpeg({
        now: new Date(),
        city: env.WALLPAPER_CITY || WALLPAPER_DEFAULT_CITY,
        weather,
        notes,
    });
    return json({ ok: true, dataUrl: `data:image/jpeg;base64,${jpegBase64}`, notes, weather, city: env.WALLPAPER_CITY || WALLPAPER_DEFAULT_CITY });
}

async function adminWallpaperPushWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const body = await request.json<{ device_id?: string }>().catch(() => ({ device_id: undefined }));
    const deviceId = body.device_id && isDeviceId(body.device_id) ? body.device_id : undefined;
    const result = await generateAndPushWallpaper(env, deviceId);
    return json({ ok: true, pushed: result.pushed });
}

async function adminWallpaperNotesWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const body = await request.json<{ text?: string }>().catch(() => null);
    if (!body || typeof body.text !== "string") return json({ error: "invalid body" }, 400);
    await saveWallpaperNotes(env, body.text);
    return json({ ok: true });
}

/* 管理后台「在线发送 Hook」：以 admin 登录态向设备推送一条通知，无需 Hook token。
 * 与 pushNotify 走同一条 DO 通道并落地 hook_notify_log，方便在没有 agent 环境时自测。 */
async function adminHookPush(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const body = await request.json<{ device_id: string; title?: string; content?: string }>().catch(() => null);
    if (!body || !isDeviceId(body.device_id) || typeof body.content !== "string" || !body.content) {
        return json({ error: "invalid hook payload" }, 400);
    }
    const sanitize = (value: unknown, maxLength: number, fallback: string): string => {
        if (typeof value !== "string") return fallback;
        return ([...value.replace(/[^ -~]/gu, "").slice(0, maxLength)]).join("") || fallback;
    };
    const title = sanitize(body.title, 32, "Agent");
    const notifyId = crypto.randomUUID();
    const ts = Math.floor(Date.now() / 1000);

    let sent = false;
    let online = false;
    let relayOk = false;
    try {
        const relay = env.PASSPORTS.get(env.PASSPORTS.idFromName(body.device_id));
        const res = await relay.fetch("https://passport.internal/internal/notify", {
            method: "POST",
            headers: { "Content-Type": "application/json", "X-Passport-Device-Id": body.device_id },
            body: JSON.stringify({ id: notifyId, title, content: body.content, ts }),
        });
        relayOk = res.ok;
        if (res.ok) {
            const data = await res.json<{ sent?: boolean; online?: boolean }>().catch(() => null);
            sent = Boolean(data?.sent);
            online = Boolean(data?.online);
        }
    } catch (err) {
        console.error("Admin hook push failed", err);
    }

    // 写入 hook 日志（与管理后台 Hook 日志一致）
    try {
        await writeHookNotifyLog(env, {
            id: notifyId, device_id: body.device_id, session_id: null,
            title, content: body.content,
            result: relayOk ? (sent ? "sent" : "offline") : "error",
            online: online ? 1 : 0, created_at: ts,
        });
    } catch (err) { console.error("Admin hook log write failed", err); }

    return json({ ok: true, sent, online, relay_ok: relayOk });
}

async function adminDeleteImageWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const body = await request.json<{ image_id: string }>().catch(() => null);
    if (!body || typeof body.image_id !== "string") return json({ error: "invalid image_id" }, 400);
    await env.DB.prepare("DELETE FROM device_images WHERE image_id = ?1").bind(body.image_id).run();
    return json({ ok: true });
}

async function adminRevokeDeviceWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return adminUnauthorizedPage();
    const formData = await request.formData().catch(() => null);
    const deviceId = formData?.get("device_id");
    if (typeof deviceId === "string" && isDeviceId(deviceId)) {
        await env.DB.prepare(
            "UPDATE devices SET status = 'revoked', previous_credential_hash = NULL, previous_credential_expires_at = NULL WHERE device_id = ?1",
        ).bind(deviceId).run();
        try {
            await env.PASSPORTS.get(env.PASSPORTS.idFromName(deviceId)).fetch("https://passport.internal/internal/revoke", {
                method: "POST",
                headers: { "X-Passport-Device-Id": deviceId },
            });
        } catch {}
    }
    return Response.redirect(new URL("/admin", request.url).toString(), 303);
}

async function adminRotateDeviceWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return adminUnauthorizedPage();
    const formData = await request.formData().catch(() => null);
    const deviceId = formData?.get("device_id");
    if (typeof deviceId === "string" && isDeviceId(deviceId)) {
        const credential = issueDeviceCredential();
        const credentialHash = await hashDeviceCredential(credential, env.DEVICE_CREDENTIAL_PEPPER);
        const now = nowSeconds();
        await env.DB.prepare(
            "UPDATE devices SET previous_credential_hash = credential_hash, previous_credential_expires_at = ?1, " +
            "credential_hash = ?2, credential_version = credential_version + 1, rotated_at = ?3 " +
            "WHERE device_id = ?4 AND status = 'active'",
        ).bind(now + 600, credentialHash, now, deviceId).run();
    }
    return Response.redirect(new URL("/admin", request.url).toString(), 303);
}

async function adminDeleteDeviceWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return adminUnauthorizedPage();
    const formData = await request.formData().catch(() => null);
    const deviceId = formData?.get("device_id");
    if (typeof deviceId === "string" && isDeviceId(deviceId)) {
        await env.DB.prepare("DELETE FROM devices WHERE device_id = ?1").bind(deviceId).run();
        await env.DB.prepare("DELETE FROM device_enrollments WHERE device_id = ?1").bind(deviceId).run();
        await env.DB.prepare("DELETE FROM device_images WHERE device_id = ?1").bind(deviceId).run();
        try {
            await env.PASSPORTS.get(env.PASSPORTS.idFromName(deviceId)).fetch("https://passport.internal/internal/revoke", {
                method: "POST",
                headers: { "X-Passport-Device-Id": deviceId },
            });
        } catch {}
    }
    return Response.redirect(new URL("/admin", request.url).toString(), 303);
}

async function listDevicesApi(request: Request, env: Env): Promise<Response> {
    const hasAdminKey = hasBearerSecret(request, env.ADMIN_API_KEY);
    const basicAuthUser = await verifyAdminBasicAuth(request, env);
    if (!hasAdminKey && !basicAuthUser) return json({ error: "unauthorized" }, 401);

    const devices = (await env.DB.prepare(
        "SELECT device_id, status, credential_version, created_at, rotated_at FROM devices ORDER BY created_at DESC",
    ).all<DeviceRow>()).results;

    const list = await Promise.all(devices.map(async (d) => {
        const online = await checkDeviceOnline(env, d.device_id);
        return {
            device_id: d.device_id,
            status: d.status,
            online,
            credential_version: d.credential_version,
            created_at: d.created_at,
            rotated_at: d.rotated_at,
        };
    }));

    return json({ devices: list });
}

async function adminPairPage(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return adminUnauthorizedPage();
    return pairingPage("设备配对", "输入设备上显示的 6 位配对码。下一步会先显示待绑定设备，再确认绑定。", "/admin/pair", "下一步：确认设备", renderSimpleNav());
}

async function adminPair(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return adminUnauthorizedPage();

    const form = await pairForm(request);
    if (!form) return pairUnavailablePage("Unsupported pairing request.", 400);
    if (form.action === "preview") return previewAdminPair(env, username, form.userCode);
    if (form.action === "confirm") return confirmAdminPair(env, username, form.confirmation);
    return pairUnavailablePage("Invalid pairing request.", 400);
}

async function pairForm(request: Request): Promise<{ action: string; userCode: string | null; confirmation: string | null } | null> {
    if (!request.headers.get("Content-Type")?.toLowerCase().startsWith("application/x-www-form-urlencoded")) return null;
    const body = await request.formData().catch(() => null);
    if (!body) return null;
    const action = body.get("action");
    const userCode = body.get("user_code");
    const confirmation = body.get("confirmation");
    return {
        action: typeof action === "string" ? action : "preview",
        userCode: typeof userCode === "string" ? normalizeUserCode(userCode) : null,
        confirmation: typeof confirmation === "string" ? confirmation : null,
    };
}

async function previewAdminPair(env: Env, subject: string, userCode: string | null): Promise<Response> {
    if (!userCode) return pairingPage("设备配对", "请输入严格的 6 位数字配对码。", "/admin/pair", "下一步：确认设备", renderSimpleNav());
    const enrollment = await enrollmentForUserCode(env, userCode);
    if (!enrollment || !await isPendingEnrollment(env, enrollment)) {
        return pairUnavailablePage("该配对码无效、已过期或已被使用。", 400);
    }
    const confirmation = await issuePairConfirmation(env, enrollment, subject);
    const deviceId = escapeHtml(enrollment.device_id);
    return htmlPage("确认绑定", `
        <div class="pair-card" style="max-width: 480px;">
            <div class="notice">匹配到待绑定设备：<br><strong style="font-size: 1.2rem; font-family: monospace; display: block; margin-top: 0.5rem;">${deviceId}</strong></div>
            <p class="desc">请确认这是您的目标设备硬件。点击下方按钮立即完成绑定授权：</p>
            <form method="post" action="/admin/pair">
                <input type="hidden" name="action" value="confirm">
                <input type="hidden" name="confirmation" value="${escapeHtml(confirmation)}">
                <button type="submit" class="btn btn-primary btn-block">✅ 确认并绑定该设备</button>
            </form>
        </div>
    `, 200, undefined, renderSimpleNav());
}

async function confirmAdminPair(
    env: Env,
    username: string,
    rawConfirmation: string | null,
): Promise<Response> {
    const confirmation = rawConfirmation ? await verifyPairConfirmation(env, rawConfirmation, username) : null;
    if (!confirmation) return pairUnavailablePage("确认信息无效或已过期。", 400);
    const enrollment = await enrollmentForConfirmation(env, confirmation);
    if (!enrollment) return pairUnavailablePage("该配对记录已不可用。", 400);
    const result = await completeEnrollmentApproval(env, username, enrollment);
    if (result.kind !== "approved") return pairUnavailablePage("该配对记录已不可用。", 400);
    return htmlPage("绑定完成", `
        <div class="pair-card" style="text-align: center;">
            <div style="font-size: 3rem; margin-bottom: 1rem;">🎉</div>
            <h2 style="margin-bottom: 0.5rem;">设备绑定成功</h2>
            <p class="desc">设备 <strong class="code-mono">${escapeHtml(result.deviceId)}</strong> 已通过授权并注册完成。</p>
            <a href="/admin" class="btn btn-primary btn-block">返回设备管理控制台</a>
        </div>
    `, 200, undefined, renderSimpleNav());
}

function adminUnauthorizedPage(): Response {
    return htmlPage(
        "Authentication required",
        "<p class=\"notice\">需要管理员身份凭证登录。</p>",
        401,
        { "WWW-Authenticate": 'Basic realm="Passport admin", charset="UTF-8"' },
    );
}

function pairUnavailablePage(message: string, status: number): Response {
    return htmlPage("设备配对", `
        <div class="pair-card" style="text-align: center;">
            <div class="notice" style="background: rgba(248, 81, 73, 0.1); border-color: rgba(248, 81, 73, 0.3); color: #f85149;">
                ${escapeHtml(message)}
            </div>
            <a href="/admin/pair" class="btn btn-primary btn-block">输入其他配对码</a>
            <a href="/admin" class="btn btn-block" style="background: transparent; color: var(--text-muted); border: 1px solid var(--border); margin-top: 0.5rem;">返回控制台</a>
        </div>
    `, status, undefined, renderSimpleNav());
}

function base64UrlEncode(bytes: Uint8Array): string {
    let binary = "";
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return btoa(binary).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");
}

function base64UrlDecode(value: string): Uint8Array | null {
    if (!/^[A-Za-z0-9_-]+$/u.test(value)) return null;
    try {
        const binary = atob(value.replaceAll("-", "+").replaceAll("_", "/") + "=".repeat((4 - value.length % 4) % 4));
        return Uint8Array.from(binary, (character) => character.charCodeAt(0));
    } catch {
        return null;
    }
}

interface PairConfirmation {
    enrollmentId: string;
    userCodeHash: string;
    subject: string;
    expiresAt: number;
}

async function pairConfirmationKey(env: Env, usages: Array<"sign" | "verify">): Promise<CryptoKey> {
    return crypto.subtle.importKey(
        "raw",
        new TextEncoder().encode(env.ADMIN_UI_PASSWORD),
        { name: "HMAC", hash: "SHA-256" },
        false,
        usages,
    );
}

async function issuePairConfirmation(env: Env, enrollment: EnrollmentRecord, subject: string): Promise<string> {
    const payload: PairConfirmation = {
        enrollmentId: enrollment.enrollment_id,
        userCodeHash: enrollment.user_code_hash,
        subject,
        expiresAt: nowSeconds() + 5 * 60,
    };
    const encoded = base64UrlEncode(new TextEncoder().encode(JSON.stringify(payload)));
    const signature = await crypto.subtle.sign("HMAC", await pairConfirmationKey(env, ["sign"]), new TextEncoder().encode(encoded));
    return `${encoded}.${base64UrlEncode(new Uint8Array(signature))}`;
}

async function verifyPairConfirmation(env: Env, raw: string, subject: string): Promise<PairConfirmation | null> {
    if (raw.length > 2048) return null;
    const parts = raw.split(".");
    if (parts.length !== 2) return null;
    const [encoded, signature] = parts;
    const signatureBytes = base64UrlDecode(signature);
    if (!signatureBytes) return null;
    const verified = await crypto.subtle.verify(
        "HMAC",
        await pairConfirmationKey(env, ["verify"]),
        signatureBytes,
        new TextEncoder().encode(encoded),
    );
    if (!verified) return null;
    const payloadBytes = base64UrlDecode(encoded);
    if (!payloadBytes) return null;
    try {
        const payload = JSON.parse(new TextDecoder().decode(payloadBytes)) as Partial<PairConfirmation>;
        const expiresAt = payload.expiresAt;
        if (typeof payload.enrollmentId !== "string" || !/^[0-9a-f-]{36}$/u.test(payload.enrollmentId) ||
            typeof payload.userCodeHash !== "string" || !/^[A-Za-z0-9_-]{43}$/u.test(payload.userCodeHash) ||
            typeof payload.subject !== "string" || payload.subject !== subject ||
            typeof expiresAt !== "number" || !Number.isInteger(expiresAt) || expiresAt <= nowSeconds()) return null;
        return payload as PairConfirmation;
    } catch {
        return null;
    }
}

async function createDeviceCode(request: Request, env: Env): Promise<Response> {
    const body = await request.json<unknown>().catch(() => null);
    if (!isExactDeviceIdBody(body)) return json({ error: "invalid device_id" }, 400);
    const deviceId = body.device_id;

    // 将该设备先前的未完成 pending 配对全部设为 expired，以便允许重新配对
    await env.DB.prepare(
        "UPDATE device_enrollments SET status = 'expired' WHERE device_id = ?1 AND status = 'pending'",
    ).bind(deviceId).run();

    const deviceCode = issueDeviceCredential();
    const deviceCodeHash = await hashEnrollmentCode(deviceCode, "device-code", env.DEVICE_CREDENTIAL_PEPPER);
    const now = nowSeconds();
    for (let attempt = 0; attempt < 5; attempt++) {
        const userCode = issueUserCode();
        try {
            await env.DB.prepare(
                "INSERT INTO device_enrollments (enrollment_id, device_id, device_code_hash, user_code_hash, expires_at, poll_interval_seconds, created_at) " +
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
            ).bind(
                crypto.randomUUID(),
                deviceId,
                deviceCodeHash,
                await hashEnrollmentCode(userCode, "user-code", env.DEVICE_CREDENTIAL_PEPPER),
                now + enrollmentLifetimeSeconds,
                enrollmentPollIntervalSeconds,
                now,
            ).run();
            return json({
                device_id: deviceId,
                device_code: deviceCode,
                user_code: userCode,
                verification_uri: new URL("/admin/pair", request.url).toString(),
                expires_in: enrollmentLifetimeSeconds,
                interval: enrollmentPollIntervalSeconds,
            }, 201);
        } catch {
            // A unique failure may be a live six-digit-code collision. Retry with a fresh code.
        }
    }
    return json({ error: "enrollment unavailable" }, 503);
}

type ApprovalResult =
    | { kind: "approved"; deviceId: string }
    | { kind: "expired" }
    | { kind: "already-approved" }
    | { kind: "state"; status: EnrollmentStatus }
    | { kind: "unavailable" };

async function enrollmentForUserCode(env: Env, userCode: string): Promise<EnrollmentRecord | null> {
    const userCodeHash = await hashEnrollmentCode(userCode, "user-code", env.DEVICE_CREDENTIAL_PEPPER);
    return env.DB.prepare(
        "SELECT enrollment_id, device_id, device_code_hash, user_code_hash, status, expires_at, poll_interval_seconds, last_polled_at " +
        "FROM device_enrollments WHERE user_code_hash = ?1 " +
        "ORDER BY CASE WHEN status IN ('pending', 'approved') THEN 0 ELSE 1 END, created_at DESC LIMIT 1",
    ).bind(userCodeHash).first<EnrollmentRecord>();
}

async function enrollmentForConfirmation(env: Env, confirmation: PairConfirmation): Promise<EnrollmentRecord | null> {
    return env.DB.prepare(
        "SELECT enrollment_id, device_id, device_code_hash, user_code_hash, status, expires_at, poll_interval_seconds, last_polled_at " +
        "FROM device_enrollments WHERE enrollment_id = ?1 AND user_code_hash = ?2",
    ).bind(confirmation.enrollmentId, confirmation.userCodeHash).first<EnrollmentRecord>();
}

async function isPendingEnrollment(env: Env, enrollment: EnrollmentRecord): Promise<boolean> {
    const now = nowSeconds();
    if (enrollment.status === "pending" && enrollment.expires_at > now) return true;
    if (enrollment.status === "pending" && enrollment.expires_at <= now) {
        await env.DB.prepare(
            "UPDATE device_enrollments SET status = 'expired' WHERE enrollment_id = ?1 AND status = 'pending'",
        ).bind(enrollment.enrollment_id).run();
    }
    return false;
}

async function completeEnrollmentApproval(
    env: Env,
    subject: string,
    enrollment: EnrollmentRecord,
): Promise<ApprovalResult> {
    const now = nowSeconds();
    if (enrollment.status === "approved") return { kind: "already-approved" };
    if (enrollment.status !== "pending") return { kind: "state", status: enrollment.status };
    if (enrollment.expires_at <= now) {
        await env.DB.prepare(
            "UPDATE device_enrollments SET status = 'expired' WHERE enrollment_id = ?1 AND status = 'pending'",
        ).bind(enrollment.enrollment_id).run();
        return { kind: "expired" };
    }

    const result = await env.DB.prepare(
        "UPDATE device_enrollments SET status = 'approved', approved_by = ?1, approved_subject = ?2, approved_at = ?3 " +
        "WHERE enrollment_id = ?4 AND status = 'pending' AND expires_at > ?3",
    ).bind(subject, subject, now, enrollment.enrollment_id).run();
    const changes = (result.meta as { changed_db_rows?: number; changes?: number }).changed_db_rows ??
                    (result.meta as { changed_db_rows?: number; changes?: number }).changes ?? 0;
    if (!changes) {
        const latest = await env.DB.prepare(
            "SELECT status FROM device_enrollments WHERE enrollment_id = ?1",
        ).bind(enrollment.enrollment_id).first<{ status: EnrollmentStatus }>();
        if (!latest) return { kind: "unavailable" };
        if (latest.status === "approved") return { kind: "already-approved" };
        if (latest.status === "expired") return { kind: "expired" };
        return { kind: "state", status: latest.status };
    }
    return { kind: "approved", deviceId: enrollment.device_id };
}

async function approveEnrollment(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) {
        return json({ error: "unauthorized" }, 401, {
            "WWW-Authenticate": 'Basic realm="Passport admin", charset="UTF-8"',
        });
    }
    const userCode = await approvedUserCode(request);
    if (!userCode) return json({ error: "invalid user_code" }, 400);

    const enrollment = await enrollmentForUserCode(env, userCode);
    if (!enrollment) return json({ error: "user_code not found" }, 404);
    const result = await completeEnrollmentApproval(env, username, enrollment);
    if (result.kind === "approved") return json({ approved: true, device_id: result.deviceId });
    if (result.kind === "already-approved") return json({ error: "user_code already approved" }, 409);
    if (result.kind === "expired") return json({ error: "user_code expired" }, 410);
    if (result.kind === "state") return json({ error: `user_code ${result.status}` }, 409);
    return json({ error: "approval unavailable" }, 503);
}

async function exchangeDeviceCode(request: Request, env: Env): Promise<Response> {
    const body = await request.json<unknown>().catch(() => null);
    if (!body || typeof body !== "object" || Array.isArray(body) || Object.keys(body).length !== 2 ||
        typeof (body as { device_id?: unknown }).device_id !== "string" ||
        typeof (body as { device_code?: unknown }).device_code !== "string") {
        return json({ error: "invalid token request" }, 400);
    }
    const deviceId = (body as { device_id: string }).device_id;
    const deviceCode = (body as { device_code: string }).device_code;
    if (!isDeviceId(deviceId) || !isExactDeviceCode(deviceCode)) return json({ error: "invalid token request" }, 400);

    const deviceCodeHash = await hashEnrollmentCode(deviceCode, "device-code", env.DEVICE_CREDENTIAL_PEPPER);
    const enrollment = await env.DB.prepare(
        "SELECT enrollment_id, device_id, device_code_hash, user_code_hash, status, expires_at, poll_interval_seconds, last_polled_at " +
        "FROM device_enrollments WHERE device_id = ?1 AND device_code_hash = ?2",
    ).bind(deviceId, deviceCodeHash).first<EnrollmentRecord>();
    if (!enrollment) return json({ error: "device_code not found" }, 404);

    const now = nowSeconds();
    if (enrollment.status === "pending") {
        if (enrollment.expires_at <= now) {
            await env.DB.prepare(
                "UPDATE device_enrollments SET status = 'expired' WHERE enrollment_id = ?1 AND status = 'pending'",
            ).bind(enrollment.enrollment_id).run();
            return json({ error: "device_code expired" }, 410);
        }
        if (enrollment.last_polled_at !== null && now - enrollment.last_polled_at < enrollment.poll_interval_seconds) {
            return json({ error: "slow_down", interval: enrollment.poll_interval_seconds }, 429);
        }
        await env.DB.prepare(
            "UPDATE device_enrollments SET last_polled_at = ?1 WHERE enrollment_id = ?2 AND status = 'pending' " +
            "AND (last_polled_at IS NULL OR ?1 - last_polled_at >= poll_interval_seconds)",
        ).bind(now, enrollment.enrollment_id).run();
        return json({ error: "authorization_pending", interval: enrollment.poll_interval_seconds }, 428);
    }

    if (enrollment.status === "approved") {
        if (enrollment.expires_at <= now) {
            await env.DB.prepare(
                "UPDATE device_enrollments SET status = 'expired' WHERE enrollment_id = ?1 AND status = 'approved'",
            ).bind(enrollment.enrollment_id).run();
            return json({ error: "device_code expired" }, 410);
        }
        const credential = issueDeviceCredential();
        const credentialHash = await hashDeviceCredential(credential, env.DEVICE_CREDENTIAL_PEPPER);

        // 1. 原子标记已消费
        const updateEnrollment = await env.DB.prepare(
            "UPDATE device_enrollments SET status = 'consumed', consumed = 1, consumed_at = ?1 " +
            "WHERE enrollment_id = ?2 AND status = 'approved' AND consumed = 0 AND expires_at > ?1",
        ).bind(now, enrollment.enrollment_id).run();

        const changes = (updateEnrollment.meta as { changed_db_rows?: number; changes?: number }).changed_db_rows ??
                        (updateEnrollment.meta as { changed_db_rows?: number; changes?: number }).changes ?? 0;

        if (changes === 0) {
            return json({ error: "credential already issued" }, 409);
        }

        // 2. 插入或覆盖写入 devices
        await env.DB.prepare(
            "INSERT INTO devices (device_id, credential_hash, credential_version, status, created_at) " +
            "VALUES (?1, ?2, 1, 'active', ?3) " +
            "ON CONFLICT(device_id) DO UPDATE SET " +
            "credential_hash = excluded.credential_hash, " +
            "previous_credential_hash = NULL, " +
            "previous_credential_expires_at = NULL, " +
            "credential_version = devices.credential_version + 1, " +
            "status = 'active', " +
            "rotated_at = excluded.created_at",
        ).bind(deviceId, credentialHash, now).run();

        return json({ device_id: deviceId, credential, credential_version: 1 }, 201);
    }

    if (enrollment.status === "expired") return json({ error: "device_code expired" }, 410);
    if (enrollment.status === "denied") return json({ error: "device_code denied" }, 403);
    return json({ error: "credential already issued" }, 409);
}

function isExactDeviceCode(value: string): boolean {
    return /^[A-Za-z0-9_-]{43}$/u.test(value);
}

function isExactDeviceIdBody(body: unknown): body is { device_id: string } {
    return Boolean(
        body && typeof body === "object" && !Array.isArray(body) && Object.keys(body).length === 1 &&
        typeof (body as { device_id?: unknown }).device_id === "string" &&
        isDeviceId((body as { device_id: string }).device_id),
    );
}

async function approvedUserCode(request: Request): Promise<string | null> {
    const contentType = request.headers.get("Content-Type")?.toLowerCase() ?? "";
    if (contentType.startsWith("application/json")) {
        const body = await request.json<unknown>().catch(() => null);
        if (!body || typeof body !== "object" || Array.isArray(body) || Object.keys(body).length !== 1 ||
            typeof (body as { user_code?: unknown }).user_code !== "string") return null;
        return normalizeUserCode((body as { user_code: string }).user_code);
    }
    if (contentType.startsWith("application/x-www-form-urlencoded")) {
        const body = await request.formData().catch(() => null);
        const value = body?.get("user_code");
        return typeof value === "string" ? normalizeUserCode(value) : null;
    }
    return null;
}

async function connectDevice(request: Request, env: Env, deviceId: string): Promise<Response> {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") return json({ error: "websocket upgrade required" }, 426);
    const credential = bearerToken(request);
    const verified = credential ? await verifyDeviceCredential(env, deviceId, credential) : null;
    if (!verified) return json({ error: "unauthorized" }, 401);

    const headers = new Headers(request.headers);
    // Overwrite, never trust externally supplied forwarding identity or credential-hash headers.
    headers.set("X-Passport-Authenticated-Device", deviceId);
    headers.set("X-Passport-Credential-Hash", verified.credentialHash);
    const id = env.PASSPORTS.idFromName(deviceId);
    return env.PASSPORTS.get(id).fetch(new Request(request, { headers }));
}

async function registerDevice(request: Request, env: Env): Promise<Response> {
    if (!hasBearerSecret(request, env.ADMIN_API_KEY)) return json({ error: "unauthorized" }, 401);
    const body = await request.json<unknown>().catch(() => null);
    if (!isExactDeviceIdBody(body)) return json({ error: "invalid device_id" }, 400);

    const deviceId = body.device_id;
    const credential = issueDeviceCredential();
    const credentialHash = await hashDeviceCredential(credential, env.DEVICE_CREDENTIAL_PEPPER);
    try {
        await env.DB.prepare(
            "INSERT INTO devices (device_id, credential_hash, credential_version, status, created_at) VALUES (?1, ?2, 1, 'active', ?3)",
        ).bind(deviceId, credentialHash, nowSeconds()).run();
    } catch {
        return json({ error: "device already registered" }, 409);
    }
    // This is the sole plaintext return of this credential. Do not log it.
    return json({ device_id: deviceId, credential, credential_version: 1 }, 201);
}

async function rotateDevice(request: Request, env: Env, deviceId: string): Promise<Response> {
    if (!hasBearerSecret(request, env.ADMIN_API_KEY)) return json({ error: "unauthorized" }, 401);
    const body = await request.json<unknown>().catch(() => ({}));
    if (!body || typeof body !== "object" || Array.isArray(body) ||
        Object.keys(body).some((key) => key !== "grace_seconds")) return json({ error: "invalid rotation request" }, 400);
    const value = (body as { grace_seconds?: unknown }).grace_seconds ?? 600;
    if (typeof value !== "number" || !Number.isInteger(value) || value < 60 || value > 3600) {
        return json({ error: "grace_seconds must be 60..3600" }, 400);
    }

    const existing = await env.DB.prepare(
        "SELECT credential_version FROM devices WHERE device_id = ?1 AND status = 'active'",
    ).bind(deviceId).first<{ credential_version: number }>();
    if (!existing) return json({ error: "device not found" }, 404);

    const credential = issueDeviceCredential();
    const credentialHash = await hashDeviceCredential(credential, env.DEVICE_CREDENTIAL_PEPPER);
    const now = nowSeconds();
    await env.DB.prepare(
        "UPDATE devices SET previous_credential_hash = credential_hash, previous_credential_expires_at = ?1, " +
        "credential_hash = ?2, credential_version = credential_version + 1, rotated_at = ?3 " +
        "WHERE device_id = ?4 AND status = 'active'",
    ).bind(now + value, credentialHash, now, deviceId).run();
    return json({
        device_id: deviceId,
        credential,
        credential_version: existing.credential_version + 1,
        grace_expires_at: now + value,
    });
}

async function revokeDevice(request: Request, env: Env, deviceId: string): Promise<Response> {
    if (!hasBearerSecret(request, env.ADMIN_API_KEY)) return json({ error: "unauthorized" }, 401);
    const result = await env.DB.prepare(
        "UPDATE devices SET status = 'revoked', previous_credential_hash = NULL, previous_credential_expires_at = NULL WHERE device_id = ?1 AND status = 'active'",
    ).bind(deviceId).run();
    if (!result.meta.changed_db_rows) return json({ error: "device not found" }, 404);

    try {
        await env.PASSPORTS.get(env.PASSPORTS.idFromName(deviceId)).fetch("https://passport.internal/internal/revoke", {
            method: "POST",
            headers: { "X-Passport-Device-Id": deviceId },
        });
    } catch (error) {
        console.error("Unable to immediately close revoked Passport session", error);
    }
    return json({ device_id: deviceId, status: "revoked" });
}

type ApprovalCreateResult =
    | { ok: true; requestId: string; status: "pending" | "allow" | "deny"; reason?: string }
    | { ok: false; error: "pending-conflict" | "unavailable" };

async function createApprovalRequest(
    env: Env,
    deviceId: string,
    input: ApprovalInput,
): Promise<ApprovalCreateResult> {
    const now = nowSeconds();
    const requestId = crypto.randomUUID();
    const expiresAt = now + input.ttlSeconds;
    await createRequestIndex(env, {
        request_id: requestId,
        device_id: deviceId,
        tool: input.tool,
        summary: input.summary,
        expires_at: expiresAt,
        created_at: now,
    });

    const relay = env.PASSPORTS.get(env.PASSPORTS.idFromName(deviceId));
    const relayResponse = await relay.fetch("https://passport.internal/internal/requests", {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-Passport-Device-Id": deviceId },
        body: JSON.stringify({ requestId, deviceId, tool: input.tool, summary: input.summary, expiresAt }),
    });
    if (relayResponse.status === 409) {
        await env.DB.prepare("DELETE FROM approval_requests WHERE request_id = ?1").bind(requestId).run();
        return { ok: false, error: "pending-conflict" };
    }
    if (!relayResponse.ok) {
        await env.DB.prepare("DELETE FROM approval_requests WHERE request_id = ?1").bind(requestId).run();
        return { ok: false, error: "unavailable" };
    }
    const result = await relayResponse.json<{ status: "pending" | "allow" | "deny"; reason?: string }>();
    if (result.status === "pending") {
        return { ok: true, requestId, status: "pending" };
    }
    return { ok: true, requestId, status: result.status, reason: result.reason };
}

async function createApproval(request: Request, env: Env, deviceId: string): Promise<Response> {
    if (!hasBearerSecret(request, env.HOOK_AUTH_SECRET)) return json({ error: "unauthorized" }, 401);
    const body = await request.json<unknown>().catch(() => null);
    const input = parseApprovalInput(body);
    if (!input) return json({ error: "invalid approval request" }, 400);
    const result = await createApprovalRequest(env, deviceId, input);
    if (!result.ok) {
        if (result.error === "pending-conflict") return json({ error: "approval already pending" }, 409);
        return json({ error: "relay unavailable" }, 503);
    }
    if (result.status === "pending") {
        return json({ request_id: result.requestId, status: "pending", expires_at: nowSeconds() + input.ttlSeconds }, 202);
    }
    return json({ request_id: result.requestId, status: result.status, reason: result.reason }, 202);
}

async function adminPushWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const body = await request.json<unknown>().catch(() => null);
    if (!body || typeof body !== "object" || Array.isArray(body)) return json({ error: "invalid body" }, 400);
    const object = body as Record<string, unknown>;
    if (typeof object.device_id !== "string" || !isDeviceId(object.device_id)) {
        return json({ error: "invalid device_id" }, 400);
    }
    const deviceId = object.device_id;
    const input = parseApprovalInput({ tool: object.tool, summary: object.summary, ttl_seconds: object.ttl_seconds });
    if (!input) return json({ error: "invalid approval request" }, 400);
    const result = await createApprovalRequest(env, deviceId, input);
    if (!result.ok) {
        if (result.error === "pending-conflict") return json({ error: "approval already pending" }, 409);
        return json({ error: "relay unavailable" }, 503);
    }
    return json({ ok: true, request_id: result.requestId, status: result.status, reason: result.reason });
}

async function adminPushStatusWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const url = new URL(request.url);
    const requestId = url.searchParams.get("request_id");
    if (!requestId || !REQUEST_ID_PATTERN.test(requestId)) return json({ error: "invalid request_id" }, 400);
    const record = await getRequestIndex(env, requestId);
    if (!record) return json({ error: "not found" }, 404);
    return json({
        request_id: record.request_id,
        status: record.status,
        reason: record.reason,
        expires_at: record.expires_at,
        decided_at: record.decided_at,
    });
}

async function pushNotify(request: Request, env: Env, deviceId: string): Promise<Response> {
    if (!hasBearerSecret(request, env.HOOK_AUTH_SECRET)) return json({ error: "unauthorized" }, 401);
    const body = await request.json<{ session_id?: string; title?: string; content?: string }>().catch(() => null);
    if (!body || typeof body.content !== "string") return json({ error: "invalid notify body" }, 400);
    const sanitize = (value: unknown, maxLength: number, fallback: string): string => {
        if (typeof value !== "string") return fallback;
        return ([...value.replace(/[^ -~]/gu, "").slice(0, maxLength)]).join("") || fallback;
    };
    const title = sanitize(body.title, 32, "Agent");
    const notifyId = crypto.randomUUID();
    const ts = Math.floor(Date.now() / 1000);

    const relay = env.PASSPORTS.get(env.PASSPORTS.idFromName(deviceId));
    const relayResponse = await relay.fetch("https://passport.internal/internal/notify", {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-Passport-Device-Id": deviceId },
        body: JSON.stringify({ id: notifyId, title, content: body.content, ts }),
    });
    if (!relayResponse.ok) {
        // Record the failed push for auditing.
        try {
            await writeHookNotifyLog(env, {
                id: notifyId, device_id: deviceId, session_id: body.session_id ?? null,
                title, content: body.content, result: "error", online: 0, created_at: ts,
            });
        } catch (err) { console.error("notify log write (error) failed", err); }
        return json({ error: "relay unavailable" }, 503);
    }

    // Persist the push outcome (delivered / device online) for the admin dashboard.
    try {
        let sent = false;
        let online = false;
        try {
            const data = await relayResponse.json<{ sent?: boolean; online?: boolean }>();
            sent = Boolean(data?.sent);
            online = Boolean(data?.online);
        } catch {}
        const result: "sent" | "offline" = sent ? "sent" : "offline";
        await writeHookNotifyLog(env, {
            id: notifyId, device_id: deviceId, session_id: body.session_id ?? null,
            title, content: body.content, result, online: online ? 1 : 0, created_at: ts,
        });
        return json({ ok: true, sent, online });
    } catch (err) {
        console.error("Hook notify log write failed", err);
        return json({ ok: false, error: "log_failed" }, 500);
    }
}

async function getApproval(request: Request, env: Env, requestId: string): Promise<Response> {
    if (!hasBearerSecret(request, env.HOOK_AUTH_SECRET)) return json({ error: "unauthorized" }, 401);
    if (!REQUEST_ID_PATTERN.test(requestId)) return json({ error: "not found" }, 404);
    const indexed = await getRequestIndex(env, requestId);
    if (!indexed) return json({ error: "not found" }, 404);
    if (indexed.status !== "pending") return json({ status: indexed.status, reason: indexed.reason });

    const relay = env.PASSPORTS.get(env.PASSPORTS.idFromName(indexed.device_id));
    return relay.fetch(`https://passport.internal/internal/requests/${requestId}`, {
        headers: { "X-Passport-Device-Id": indexed.device_id },
    });
}

async function adminScreencastWs(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return adminUnauthorizedPage();
    const url = new URL(request.url);
    const deviceId = url.searchParams.get("deviceId") || url.searchParams.get("device_id");
    if (!deviceId || !isDeviceId(deviceId)) return json({ error: "invalid device_id" }, 400);

    const id = env.PASSPORTS.idFromName(deviceId);
    return env.PASSPORTS.get(id).fetch("https://passport.internal/internal/screencast/ws", {
        headers: request.headers,
    });
}

async function adminScreencastCommand(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const body = await request.json<{ deviceId: string; action: string; [key: string]: unknown }>().catch(() => null);
    if (!body || !isDeviceId(body.deviceId)) return json({ error: "invalid body" }, 400);

    const id = env.PASSPORTS.idFromName(body.deviceId);
    const commandPayload = JSON.stringify({
        v: 1,
        type: body.action,
        ...body,
    });
    return env.PASSPORTS.get(id).fetch("https://passport.internal/internal/screencast/command", {
        method: "POST",
        body: commandPayload,
    });
}

async function adminScreencastLatest(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const url = new URL(request.url);
    const deviceId = url.searchParams.get("deviceId") || url.searchParams.get("device_id");
    if (!deviceId || !isDeviceId(deviceId)) return json({ error: "invalid device_id" }, 400);

    const id = env.PASSPORTS.idFromName(deviceId);
    return env.PASSPORTS.get(id).fetch("https://passport.internal/internal/screencast/latest");
}

