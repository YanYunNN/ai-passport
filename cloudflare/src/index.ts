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
    max-width: 900px;
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
        "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'",
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

    const deviceList = await Promise.all(devices.map(async (d) => {
        const online = await checkDeviceOnline(env, d.device_id);
        return { ...d, online };
    }));

    const totalCount = deviceList.length;
    const onlineCount = deviceList.filter((d) => d.online).length;
    const activeCount = deviceList.filter((d) => d.status === "active").length;
    const pendingCount = pendings.length;

    let devicesRows = "";
    if (deviceList.length === 0) {
        devicesRows = `<tr><td colspan="6" class="empty-state">暂无已绑定的设备。点击上方「配对新设备」开始添加。</td></tr>`;
    } else {
        for (const d of deviceList) {
            const statusBadge = d.status === "active"
                ? `<span class="badge badge-active">Active</span>`
                : `<span class="badge badge-revoked">Revoked</span>`;
            const onlineDot = d.online
                ? `<span class="badge badge-active"><span class="dot dot-online"></span> 在线</span>`
                : `<span class="badge" style="color: var(--text-muted);"><span class="dot dot-offline"></span> 离线</span>`;

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

    let pendingSection = "";
    if (pendings.length > 0) {
        let pendingRows = "";
        for (const p of pendings) {
            const timeLeft = Math.max(0, p.expires_at - now);
            pendingRows += `
            <tr>
                <td><span class="code-mono">${escapeHtml(p.device_id)}</span></td>
                <td>${formatDate(p.created_at)}</td>
                <td>剩余 ${timeLeft} 秒</td>
                <td><a href="/admin/pair" class="btn btn-sm btn-primary">输入配对码绑定</a></td>
            </tr>`;
        }
        pendingSection = `
        <div class="card" style="border-color: rgba(56, 139, 253, 0.4);">
            <div class="card-header" style="background: rgba(56, 139, 253, 0.08);">
                <div class="card-title" style="color: #58a6ff;">⏳ 待配对申请 (${pendingCount})</div>
            </div>
            <table>
                <thead>
                    <tr>
                        <th>申请设备 ID</th>
                        <th>申请时间</th>
                        <th>有效倒计时</th>
                        <th>操作</th>
                    </tr>
                </thead>
                <tbody>${pendingRows}</tbody>
            </table>
        </div>`;
    }

    const content = `
        <header>
            <div>
                <h1>Kiro Passport Relay 管理控制台</h1>
                <p style="color: var(--text-muted); font-size: 0.85rem; margin-top: 0.25rem;">已登录: <strong>${escapeHtml(username)}</strong></p>
            </div>
            <div class="header-actions">
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

        ${pendingSection}

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
    `;

    return htmlPage("管理控制台", content);
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
        "SELECT device_id, status, credential_version, created_at, rotated_at FROM devices ORDER BY created_at DESC"
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
    const registered = await env.DB.prepare("SELECT 1 FROM devices WHERE device_id = ?1").bind(deviceId).first();
    if (registered) return json({ error: "device already registered" }, 409);

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
    if (!result.meta.changed_db_rows) {
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
        const batch = await env.DB.batch([
            env.DB.prepare(
                "INSERT INTO devices (device_id, credential_hash, credential_version, status, created_at) " +
                "SELECT device_id, ?1, 1, 'active', ?2 FROM device_enrollments e " +
                "WHERE e.enrollment_id = ?3 AND e.status = 'approved' AND e.consumed = 0 AND e.expires_at > ?2",
            ).bind(credentialHash, now, enrollment.enrollment_id),
            env.DB.prepare(
                "UPDATE device_enrollments SET status = 'consumed', consumed = 1, consumed_at = ?1 " +
                "WHERE enrollment_id = ?2 AND status = 'approved' AND consumed = 0 AND expires_at > ?1 " +
                "AND EXISTS (SELECT 1 FROM devices d WHERE d.device_id = device_enrollments.device_id AND d.credential_hash = ?3)",
            ).bind(now, enrollment.enrollment_id, credentialHash),
        ]);
        const created = batch[0].meta.changed_db_rows === 1;
        const consumed = batch[1].meta.changed_db_rows === 1;
        if (!created || !consumed) return json({ error: "credential already issued" }, 409);
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
