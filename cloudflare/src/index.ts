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
import { createRequestIndex, getRequestIndex } from "./db";
import type { Env } from "./env";
import { PassportRelay } from "./passport-relay";
import { isDeviceId, parseApprovalInput, REQUEST_ID_PATTERN } from "./protocol";
import { handleSimulatorBinProxy, handleSimulatorPresets, simulatorPage } from "./simulator";

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
            const statusMatch = url.pathname.match(/^\/v1\/requests\/([0-9a-f-]{1,36})$/u);
            if (request.method === "GET" && statusMatch) return getApproval(request, env, statusMatch[1]);
            return json({ error: "not found" }, 404);
        } catch (error) {
            console.error("Unhandled relay error", error);
            return json({ error: "relay unavailable" }, 503);
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

function formatDate(timestamp: number | null): string {
    if (!timestamp) return "-";
    const date = new Date(timestamp * 1000);
    return date.toISOString().replace("T", " ").slice(0, 19) + " UTC";
}

function activationPage(): Response {
    return pairingPage(
        "Pair Passport device",
        "Enter the 6-digit code shown by your device. This legacy page approves immediately after submission.",
        "/activate",
        "Approve device",
    );
}

function pairingPage(title: string, message: string, action: string, button: string): Response {
    return htmlPage(title, `
        <div class="nav-bar">
            <a href="/admin" class="nav-link">← 返回管理控制台</a>
        </div>
        <p class="desc">${message}</p>
        <form method="post" action="${action}" class="pair-card">
            <label for="user_code" class="form-label">设备 6 位配对码</label>
            <input id="user_code" name="user_code" inputmode="numeric" pattern="[0-9]{6}" autocomplete="one-time-code" spellcheck="false" maxlength="6" placeholder="000000" class="code-input" required autofocus>
            <button type="submit" class="btn btn-primary btn-block">${button}</button>
        </form>
    `);
}

function htmlPage(title: string, content: string, status = 200, additionalHeaders?: HeadersInit): Response {
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
    --danger: #da3633;
    --danger-hover: #f85149;
    --online: #3fb950;
    --offline: #6e7681;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background: var(--bg);
    color: var(--text);
    line-height: 1.5;
    padding: 2rem 1rem;
}
.container {
    max-width: 960px;
    margin: 0 auto;
}
header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 2rem;
    padding-bottom: 1rem;
    border-bottom: 1px solid var(--border);
    flex-wrap: wrap;
    gap: 1rem;
}
h1 { font-size: 1.5rem; font-weight: 600; }
.header-actions { display: flex; gap: 0.75rem; align-items: center; }
.nav-bar { margin-bottom: 1.5rem; }
.nav-link { color: var(--accent); text-decoration: none; font-size: 0.9rem; }
.nav-link:hover { text-decoration: underline; }
.stats-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 1rem;
    margin-bottom: 2rem;
}
.stat-card {
    background: var(--card-bg);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 1.25rem;
}
.stat-label { color: var(--text-muted); font-size: 0.85rem; margin-bottom: 0.25rem; }
.stat-value { font-size: 1.75rem; font-weight: 700; }
.card {
    background: var(--card-bg);
    border: 1px solid var(--border);
    border-radius: 8px;
    overflow: hidden;
    margin-bottom: 2rem;
}
.card-header {
    padding: 1rem 1.25rem;
    border-bottom: 1px solid var(--border);
    display: flex;
    justify-content: space-between;
    align-items: center;
}
.card-title { font-size: 1.1rem; font-weight: 600; }
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
.pair-card { max-width: 420px; margin: 2rem auto; background: var(--card-bg); border: 1px solid var(--border); border-radius: 8px; padding: 2rem; }
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
}
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
</style>
</head>
<body>
<div class="container">
    ${content}
</div>
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

async function adminDashboardPage(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return adminUnauthorizedPage();

    const devices = (await env.DB.prepare(
        "SELECT device_id, status, credential_version, created_at, rotated_at FROM devices ORDER BY created_at DESC"
    ).all<DeviceRow>()).results;

    const now = nowSeconds();
    const pendings = (await env.DB.prepare(
        "SELECT enrollment_id, device_id, status, expires_at, created_at FROM device_enrollments WHERE status = 'pending' AND expires_at > ?1 ORDER BY created_at DESC"
    ).bind(now).all<PendingEnrollmentRow>()).results;

    const images = (await env.DB.prepare(
        "SELECT image_id, device_id, title, image_data, created_at FROM device_images ORDER BY created_at DESC LIMIT 12"
    ).all<DeviceImageRow>()).results;

    const deviceList = await Promise.all(devices.map(async (d) => {
        const online = await checkDeviceOnline(env, d.device_id);
        return { ...d, online };
    }));

    const totalCount = deviceList.length;
    const onlineCount = deviceList.filter((d) => d.online).length;
    const activeCount = deviceList.filter((d) => d.status === "active").length;
    const pendingCount = pendings.length;

    let devicesRows = "";
    let deviceOptions = "";
    if (deviceList.length === 0) {
        devicesRows = `<tr><td colspan="6" class="empty-state">暂无已绑定的设备。点击上方「配对新设备」开始添加。</td></tr>`;
        deviceOptions = `<option value="">请先配对设备</option>`;
    } else {
        for (const d of deviceList) {
            const statusBadge = d.status === "active"
                ? `<span class="badge badge-active">Active</span>`
                : `<span class="badge badge-revoked">Revoked</span>`;
            const onlineDot = d.online
                ? `<span class="badge badge-active"><span class="dot dot-online"></span> 在线</span>`
                : `<span class="badge" style="color: var(--text-muted);"><span class="dot dot-offline"></span> 离线</span>`;

            deviceOptions += `<option value="${escapeHtml(d.device_id)}">${escapeHtml(d.device_id)} (${d.online ? '🟢 在线' : '⚪ 离线'})</option>`;

            devicesRows += `
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
    }

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
        <header>
            <div>
                <h1>Kiro Passport Relay 管理控制台</h1>
                <p style="color: var(--text-muted); font-size: 0.85rem; margin-top: 0.25rem;">已登录: <strong>${escapeHtml(username)}</strong></p>
            </div>
            <div class="header-actions">
                <a href="/simulator" class="btn btn-accent" target="_blank">🎮 Web 模拟器</a>
                <a href="/admin" class="btn" style="background: #21262d; border: 1px solid var(--border); color: var(--text);">🔄 刷新</a>
                <a href="/admin/pair" class="btn btn-primary">➕ 配对新设备</a>
            </div>
        </header>

        <div class="stats-grid">
            <div class="stat-card">
                <div class="stat-label">总设备数</div>
                <div class="stat-value">${totalCount}</div>
            </div>
            <div class="stat-card">
                <div class="stat-label">在线设备</div>
                <div class="stat-value" style="color: var(--online);">${onlineCount}</div>
            </div>
            <div class="stat-card">
                <div class="stat-label">活跃设备</div>
                <div class="stat-value">${activeCount}</div>
            </div>
            <div class="stat-card">
                <div class="stat-label">待处理配对</div>
                <div class="stat-value" style="color: #58a6ff;">${pendingCount}</div>
            </div>
        </div>

        <!-- 📸 远程屏幕快照与截屏 (Remote Screen Capture Studio) -->
        <div class="card">
            <div class="card-header">
                <div class="card-title">📸 远程屏幕快照与截屏 (Remote Snapshot - 240×320)</div>
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

        <!-- 📸 图片工作台 (Image Studio) -->
        <div class="card">
            <div class="card-header">
                <div class="card-title">📸 图片推送工作台 (Image Studio - 240×320)</div>
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

        <div class="card">
            <div class="card-header">
                <div class="card-title">📱 已注册设备列表</div>
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
                <tbody>${devicesRows}</tbody>
            </table>
        </div>

        <script>
        // ----------------- 远程屏幕快照与截屏 (Remote Snapshot Studio) -----------------
        let castWs = null;
        let castAutoTimer = null;
        let castCurrentSeq = 0;

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

        if (castTargetDevice.value) {
            setTimeout(initCastWebSocket, 300);
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
        </script>
    `;

    return htmlPage("管理控制台", content);
}

async function adminPushImageWeb(request: Request, env: Env): Promise<Response> {
    const username = await verifyAdminBasicAuth(request, env);
    if (!username) return json({ error: "unauthorized" }, 401);
    const body = await request.json<{ device_id: string; title?: string; image_data: string }>().catch(() => null);
    if (!body || !isDeviceId(body.device_id) || typeof body.image_data !== "string" || !body.image_data) {
        return json({ error: "invalid image data" }, 400);
    }
    const imageId = crypto.randomUUID();
    const now = nowSeconds();
    const title = (body.title || "Image").slice(0, 64);

    let rawBase64 = body.image_data;
    if (rawBase64.includes(",")) {
        rawBase64 = rawBase64.split(",")[1];
    }

    await env.DB.prepare(
        "INSERT INTO device_images (image_id, device_id, title, image_data, created_at) VALUES (?1, ?2, ?3, ?4, ?5)",
    ).bind(imageId, body.device_id, title, rawBase64, now).run();

    let sent = false;
    let online = false;
    try {
        const relay = env.PASSPORTS.get(env.PASSPORTS.idFromName(body.device_id));
        const res = await relay.fetch("https://passport.internal/internal/send-image", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ imageId, title, data: rawBase64 }),
        });
        if (res.ok) {
            const data = await res.json<{ sent?: boolean; online?: boolean }>();
            sent = Boolean(data?.sent);
            online = Boolean(data?.online);
        }
    } catch (err) {
        console.error("Relay send-image failed", err);
    }

    return json({ ok: true, image_id: imageId, sent, online });
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
    return pairingPage("设备配对", "输入设备上显示的 6 位配对码。下一步会先显示待绑定设备，再确认绑定。", "/admin/pair", "下一步：确认设备");
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
    if (!userCode) return pairingPage("设备配对", "请输入严格的 6 位数字配对码。", "/admin/pair", "下一步：确认设备");
    const enrollment = await enrollmentForUserCode(env, userCode);
    if (!enrollment || !await isPendingEnrollment(env, enrollment)) {
        return pairUnavailablePage("该配对码无效、已过期或已被使用。", 400);
    }
    const confirmation = await issuePairConfirmation(env, enrollment, subject);
    const deviceId = escapeHtml(enrollment.device_id);
    return htmlPage("确认绑定", `
        <div class="nav-bar">
            <a href="/admin/pair" class="nav-link">← 重新输入配对码</a>
        </div>
        <div class="pair-card" style="max-width: 480px;">
            <div class="notice">匹配到待绑定设备：<br><strong style="font-size: 1.2rem; font-family: monospace; display: block; margin-top: 0.5rem;">${deviceId}</strong></div>
            <p class="desc">请确认这是您的目标设备硬件。点击下方按钮立即完成绑定授权：</p>
            <form method="post" action="/admin/pair">
                <input type="hidden" name="action" value="confirm">
                <input type="hidden" name="confirmation" value="${escapeHtml(confirmation)}">
                <button type="submit" class="btn btn-primary btn-block">✅ 确认并绑定该设备</button>
            </form>
        </div>
    `);
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
    `);
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
    `, status);
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

async function createApproval(request: Request, env: Env, deviceId: string): Promise<Response> {
    if (!hasBearerSecret(request, env.HOOK_AUTH_SECRET)) return json({ error: "unauthorized" }, 401);
    const body = await request.json<unknown>().catch(() => null);
    const input = parseApprovalInput(body);
    if (!input) return json({ error: "invalid approval request" }, 400);

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
        return json({ error: "approval already pending" }, 409);
    }
    if (!relayResponse.ok) {
        await env.DB.prepare("DELETE FROM approval_requests WHERE request_id = ?1").bind(requestId).run();
        return json({ error: "relay unavailable" }, 503);
    }
    const result = await relayResponse.json<{ status: "pending" | "allow" | "deny"; reason?: string }>();
    if (result.status === "pending") {
        return json({ request_id: requestId, status: "pending", expires_at: expiresAt }, 202);
    }
    return json({ request_id: requestId, status: result.status, reason: result.reason }, 202);
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

