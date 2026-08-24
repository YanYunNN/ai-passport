import {
    bearerToken,
    hashDeviceCredential,
    hashEnrollmentCode,
    hasBearerSecret,
    issueDeviceCredential,
    issueUserCode,
    normalizeUserCode,
    verifyDeviceCredential,
} from "./auth";
import { verifyAccessAssertion } from "./access";
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

const json = (body: unknown, status = 200): Response => new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" },
});

const nowSeconds = (): number => Math.floor(Date.now() / 1000);

export default {
    async fetch(request: Request, env: Env): Promise<Response> {
        try {
            const url = new URL(request.url);
            if (request.method === "GET" && url.pathname === "/healthz") return json({ ok: true });

            if (request.method === "GET" && url.pathname === "/activate") return activationPage();
            if (request.method === "POST" && url.pathname === "/activate") return approveEnrollment(request, env);
            if (request.method === "POST" && url.pathname === "/v1/enrollment/device-code") {
                return createDeviceCode(request, env);
            }
            if (request.method === "POST" && url.pathname === "/v1/enrollment/token") return exchangeDeviceCode(request, env);
            if (request.method === "POST" && url.pathname === "/v1/enrollment/approve") return approveEnrollment(request, env);

            const deviceMatch = url.pathname.match(/^\/device\/(passport-[A-F0-9]{12})$/u);
            if (request.method === "GET" && deviceMatch) return connectDevice(request, env, deviceMatch[1]);

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

function activationPage(): Response {
    const body = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Passport device pairing</title><style>body{font:16px system-ui,sans-serif;max-width:32rem;margin:3rem auto;padding:0 1rem;color:#172033}label,input,button{display:block;width:100%;box-sizing:border-box}input{font:1.2rem ui-monospace,monospace;letter-spacing:.08em;margin:.5rem 0 1rem;padding:.7rem}button{padding:.75rem;background:#163b70;color:#fff;border:0;border-radius:.3rem}p{line-height:1.5;color:#43506a}</style></head>
<body><main><h1>Pair Passport device</h1><p>Enter the code shown by your device. Only approve a code you initiated.</p><form method="post" action="/activate"><label for="user_code">Pairing code</label><input id="user_code" name="user_code" autocomplete="one-time-code" autocapitalize="characters" spellcheck="false" maxlength="12" required><button type="submit">Approve device</button></form></main></body></html>`;
    return new Response(body, {
        headers: {
            "Content-Type": "text/html; charset=utf-8",
            "Cache-Control": "no-store",
            "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'",
            "Referrer-Policy": "no-referrer",
            "X-Content-Type-Options": "nosniff",
        },
    });
}

async function createDeviceCode(request: Request, env: Env): Promise<Response> {
    const body = await request.json<unknown>().catch(() => null);
    if (!isExactDeviceIdBody(body)) return json({ error: "invalid device_id" }, 400);
    const deviceId = body.device_id;
    const registered = await env.DB.prepare("SELECT 1 FROM devices WHERE device_id = ?1").bind(deviceId).first();
    if (registered) return json({ error: "device already registered" }, 409);

    const deviceCode = issueDeviceCredential();
    const rawUserCode = issueUserCode();
    const now = nowSeconds();
    const enrollmentId = crypto.randomUUID();
    try {
        await env.DB.prepare(
            "INSERT INTO device_enrollments (enrollment_id, device_id, device_code_hash, user_code_hash, expires_at, poll_interval_seconds, created_at) " +
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
        ).bind(
            enrollmentId,
            deviceId,
            await hashEnrollmentCode(deviceCode, "device-code", env.DEVICE_CREDENTIAL_PEPPER),
            await hashEnrollmentCode(rawUserCode, "user-code", env.DEVICE_CREDENTIAL_PEPPER),
            now + enrollmentLifetimeSeconds,
            enrollmentPollIntervalSeconds,
            now,
        ).run();
    } catch {
        // The partial unique index is the concurrency control for each device's live pairing.
        return json({ error: "enrollment already pending" }, 409);
    }
    return json({
        device_id: deviceId,
        device_code: deviceCode,
        user_code: formatUserCode(rawUserCode),
        verification_uri: new URL("/activate", request.url).toString(),
        expires_in: enrollmentLifetimeSeconds,
        interval: enrollmentPollIntervalSeconds,
    }, 201);
}

async function approveEnrollment(request: Request, env: Env): Promise<Response> {
    const identity = await verifyAccessAssertion(request, env);
    if (!identity) return json({ error: "forbidden" }, 403);
    const userCode = await approvedUserCode(request);
    if (!userCode) return json({ error: "invalid user_code" }, 400);

    const userCodeHash = await hashEnrollmentCode(userCode, "user-code", env.DEVICE_CREDENTIAL_PEPPER);
    const enrollment = await env.DB.prepare(
        "SELECT enrollment_id, device_id, device_code_hash, user_code_hash, status, expires_at, poll_interval_seconds, last_polled_at " +
        "FROM device_enrollments WHERE user_code_hash = ?1",
    ).bind(userCodeHash).first<EnrollmentRecord>();
    if (!enrollment) return json({ error: "invalid user_code" }, 400);

    const now = nowSeconds();
    if (enrollment.expires_at <= now) {
        await expireEnrollment(env, enrollment.enrollment_id, now);
        return json({ error: "expired_token" }, 410);
    }
    if (enrollment.status === "approved") return json({ error: "enrollment already approved" }, 409);
    if (enrollment.status !== "pending") return enrollmentStateResponse(enrollment.status);

    const result = await env.DB.prepare(
        "UPDATE device_enrollments SET status = 'approved', approved_by = ?1, approved_subject = ?2, approved_at = ?3 " +
        "WHERE enrollment_id = ?4 AND status = 'pending' AND expires_at > ?3",
    ).bind(identity.email, identity.subject, now, enrollment.enrollment_id).run();
    if (result.meta.changed_db_rows !== 1) return json({ error: "enrollment unavailable" }, 409);
    return json({ device_id: enrollment.device_id, status: "approved" });
}

async function exchangeDeviceCode(request: Request, env: Env): Promise<Response> {
    const body = await request.json<unknown>().catch(() => null);
    if (!isDeviceTokenBody(body)) return json({ error: "invalid device_code request" }, 400);

    const codeHash = await hashEnrollmentCode(body.device_code, "device-code", env.DEVICE_CREDENTIAL_PEPPER);
    const enrollment = await env.DB.prepare(
        "SELECT enrollment_id, device_id, device_code_hash, user_code_hash, status, expires_at, poll_interval_seconds, last_polled_at " +
        "FROM device_enrollments WHERE device_id = ?1 AND device_code_hash = ?2",
    ).bind(body.device_id, codeHash).first<EnrollmentRecord>();
    if (!enrollment) return json({ error: "invalid_grant" }, 400);

    const now = nowSeconds();
    if (enrollment.expires_at <= now) {
        await expireEnrollment(env, enrollment.enrollment_id, now);
        return json({ error: "expired_token" }, 410);
    }
    if (enrollment.status === "pending") {
        if (enrollment.last_polled_at !== null && enrollment.last_polled_at > now - enrollment.poll_interval_seconds) {
            return json({ error: "slow_down", interval: enrollment.poll_interval_seconds }, 429);
        }
        const polled = await env.DB.prepare(
            "UPDATE device_enrollments SET last_polled_at = ?1 WHERE enrollment_id = ?2 AND status = 'pending' " +
            "AND (last_polled_at IS NULL OR last_polled_at <= ?3)",
        ).bind(now, enrollment.enrollment_id, now - enrollment.poll_interval_seconds).run();
        if (polled.meta.changed_db_rows !== 1) return json({ error: "slow_down", interval: enrollment.poll_interval_seconds }, 429);
        return json({ error: "authorization_pending", interval: enrollment.poll_interval_seconds }, 428);
    }
    if (enrollment.status !== "approved") return enrollmentStateResponse(enrollment.status);

    // Both statements are committed as one D1 batch. The second statement requires the
    // exact credential inserted by the first, so a racing registration cannot consume a code.
    const credential = issueDeviceCredential();
    const credentialHash = await hashDeviceCredential(credential, env.DEVICE_CREDENTIAL_PEPPER);
    const results = await env.DB.batch([
        env.DB.prepare(
            "INSERT INTO devices (device_id, credential_hash, credential_version, status, created_at) " +
            "SELECT device_id, ?1, 1, 'active', ?2 FROM device_enrollments e " +
            "WHERE e.enrollment_id = ?3 AND e.device_id = ?4 AND e.device_code_hash = ?5 " +
            "AND e.status = 'approved' AND e.expires_at > ?2 " +
            "AND NOT EXISTS (SELECT 1 FROM devices d WHERE d.device_id = e.device_id)",
        ).bind(credentialHash, now, enrollment.enrollment_id, body.device_id, codeHash),
        env.DB.prepare(
            "UPDATE device_enrollments SET status = 'consumed', consumed = 1, consumed_at = ?1 " +
            "WHERE enrollment_id = ?2 AND device_id = ?3 AND device_code_hash = ?4 " +
            "AND status = 'approved' AND expires_at > ?1 " +
            "AND EXISTS (SELECT 1 FROM devices d WHERE d.device_id = device_enrollments.device_id AND d.credential_hash = ?5)",
        ).bind(now, enrollment.enrollment_id, body.device_id, codeHash, credentialHash),
    ]);
    if (results[1]?.meta.changed_db_rows !== 1) return json({ error: "invalid_grant" }, 400);
    // This is the only plaintext return of an enrollment credential. Do not log it.
    return json({ device_id: body.device_id, credential, credential_version: 1 }, 201);
}

async function expireEnrollment(env: Env, enrollmentId: string, now: number): Promise<void> {
    await env.DB.prepare(
        "UPDATE device_enrollments SET status = 'expired' WHERE enrollment_id = ?1 " +
        "AND status IN ('pending', 'approved') AND expires_at <= ?2",
    ).bind(enrollmentId, now).run();
}

function enrollmentStateResponse(status: Exclude<EnrollmentStatus, "pending" | "approved">): Response {
    if (status === "denied") return json({ error: "access_denied" }, 403);
    if (status === "expired") return json({ error: "expired_token" }, 410);
    return json({ error: "invalid_grant" }, 400);
}

function isExactDeviceIdBody(body: unknown): body is { device_id: string } {
    return !!body && typeof body === "object" && !Array.isArray(body) && Object.keys(body).length === 1 &&
        Object.hasOwn(body, "device_id") && typeof (body as { device_id: unknown }).device_id === "string" &&
        isDeviceId((body as { device_id: string }).device_id);
}

function isDeviceTokenBody(body: unknown): body is { device_id: string; device_code: string } {
    return !!body && typeof body === "object" && !Array.isArray(body) && Object.keys(body).length === 2 &&
        Object.hasOwn(body, "device_id") && Object.hasOwn(body, "device_code") &&
        typeof (body as { device_id: unknown }).device_id === "string" &&
        typeof (body as { device_code: unknown }).device_code === "string" &&
        isDeviceId((body as { device_id: string }).device_id) &&
        /^[A-Za-z0-9_-]{43}$/.test((body as { device_code: string }).device_code);
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

function formatUserCode(code: string): string {
    return `${code.slice(0, 5)}-${code.slice(5)}`;
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
        // The persisted revoked status is still checked before any future allow; DO retry on next activity is safe.
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
