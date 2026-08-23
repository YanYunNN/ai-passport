import {
    bearerToken,
    hashDeviceCredential,
    hasBearerSecret,
    issueDeviceCredential,
    verifyDeviceCredential,
} from "./auth";
import { createRequestIndex, getRequestIndex } from "./db";
import type { Env } from "./env";
import { PassportRelay } from "./passport-relay";
import { isDeviceId, parseApprovalInput, REQUEST_ID_PATTERN, SESSION_ID_PATTERN } from "./protocol";

export { PassportRelay };

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

            const deviceMatch = url.pathname.match(/^\/device\/(passport-[A-F0-9]{12})$/u);
            if (request.method === "GET" && deviceMatch) return connectDevice(request, env, deviceMatch[1]);

            if (request.method === "POST" && url.pathname === "/v1/admin/devices") {
                return registerDevice(request, env);
            }
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

async function connectDevice(request: Request, env: Env, deviceId: string): Promise<Response> {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") return json({ error: "websocket upgrade required" }, 426);
    const credential = bearerToken(request);
    if (!credential || !(await verifyDeviceCredential(env, deviceId, credential))) return json({ error: "unauthorized" }, 401);

    const headers = new Headers(request.headers);
    // Overwrite, never trust an externally supplied forwarding identity header.
    headers.set("X-Passport-Authenticated-Device", deviceId);
    const id = env.PASSPORTS.idFromName(deviceId);
    return env.PASSPORTS.get(id).fetch(new Request(request, { headers }));
}

async function registerDevice(request: Request, env: Env): Promise<Response> {
    if (!hasBearerSecret(request, env.ADMIN_API_KEY)) return json({ error: "unauthorized" }, 401);
    const body = await request.json<unknown>().catch(() => null);
    if (!body || typeof body !== "object" || Array.isArray(body) || Object.keys(body).length !== 1 ||
        !Object.hasOwn(body, "device_id") || typeof (body as { device_id: unknown }).device_id !== "string" ||
        !isDeviceId((body as { device_id: string }).device_id)) return json({ error: "invalid device_id" }, 400);

    const deviceId = (body as { device_id: string }).device_id;
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

    const credential = issueDeviceCredential();
    const credentialHash = await hashDeviceCredential(credential, env.DEVICE_CREDENTIAL_PEPPER);
    const now = nowSeconds();
    const result = await env.DB.prepare(
        "UPDATE devices SET previous_credential_hash = credential_hash, previous_credential_expires_at = ?1, " +
        "credential_hash = ?2, credential_version = credential_version + 1, rotated_at = ?3 " +
        "WHERE device_id = ?4 AND status = 'active'",
    ).bind(now + value, credentialHash, now, deviceId).run();
    if (!result.meta.changed_db_rows) return json({ error: "device not found" }, 404);
    return json({ device_id: deviceId, credential, credential_version: undefined, grace_expires_at: now + value });
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
