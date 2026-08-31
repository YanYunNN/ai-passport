import type { Env } from "./env";

export type ApprovalStatus = "pending" | "allow" | "deny";
export type DenyReason = "user" | "policy" | "timeout" | "offline" | "session_lost" | "protocol_error";

export interface RequestIndex {
    request_id: string;
    device_id: string;
    tool: string;
    summary: string;
    expires_at: number;
    status: ApprovalStatus;
    reason: DenyReason | null;
    created_at: number;
    decided_at: number | null;
}

export async function createRequestIndex(
    env: Env,
    request: Pick<RequestIndex, "request_id" | "device_id" | "tool" | "summary" | "expires_at" | "created_at">,
): Promise<void> {
    await env.DB.prepare(
        "INSERT INTO approval_requests (request_id, device_id, tool, summary, expires_at, created_at) " +
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
    ).bind(request.request_id, request.device_id, request.tool, request.summary, request.expires_at, request.created_at).run();
}

export async function getRequestIndex(env: Env, requestId: string): Promise<RequestIndex | null> {
    return env.DB.prepare(
        "SELECT request_id, device_id, tool, summary, expires_at, status, reason, created_at, decided_at " +
        "FROM approval_requests WHERE request_id = ?1",
    ).bind(requestId).first<RequestIndex>();
}

/**
 * Atomically records an outcome in the public D1 projection. An allow is only
 * claimable while the exact socket credential remains current (or a live previous
 * credential), the device is active, and the approval is still pending/unexpired.
 */
export async function claimTerminalState(
    env: Env,
    request: Pick<RequestIndex, "request_id" | "device_id" | "expires_at">,
    status: Exclude<ApprovalStatus, "pending">,
    reason: DenyReason | "user",
    decidedAt: number,
    credentialHash?: string,
): Promise<boolean> {
    const statement = status === "allow"
        ? env.DB.prepare(
            "UPDATE approval_requests SET status = 'allow', reason = ?1, decided_at = ?2 " +
            "WHERE request_id = ?3 AND device_id = ?4 AND status = 'pending' AND expires_at > ?5 " +
            "AND EXISTS (SELECT 1 FROM devices d WHERE d.device_id = approval_requests.device_id " +
            "AND d.status = 'active' AND (d.credential_hash = ?6 OR " +
            "(d.previous_credential_hash = ?6 AND d.previous_credential_expires_at >= ?5)))",
        ).bind(reason, decidedAt, request.request_id, request.device_id, decidedAt, credentialHash ?? "")
        : env.DB.prepare(
            "UPDATE approval_requests SET status = 'deny', reason = ?1, decided_at = ?2 " +
            "WHERE request_id = ?3 AND device_id = ?4 AND status = 'pending'",
        ).bind(reason, decidedAt, request.request_id, request.device_id);
    const result = await statement.run();
    return result.meta.changed_db_rows === 1;
}

export async function forceDenyAfterUncertainAllow(
    env: Env,
    request: Pick<RequestIndex, "request_id" | "device_id">,
    decidedAt: number,
): Promise<boolean> {
    // If the allow update committed but its response was lost, this compensates only
    // that exact decision timestamp. A pending row is also safely transitioned to deny.
    const result = await env.DB.prepare(
        "UPDATE approval_requests SET status = 'deny', reason = 'policy', decided_at = ?1 " +
        "WHERE request_id = ?2 AND device_id = ?3 AND " +
        "(status = 'pending' OR (status = 'allow' AND decided_at = ?1))",
    ).bind(decidedAt, request.request_id, request.device_id).run();
    return result.meta.changed_db_rows === 1;
}

export async function terminalStateMatches(
    env: Env,
    requestId: string,
    status: Exclude<ApprovalStatus, "pending">,
    reason: DenyReason | "user",
): Promise<boolean> {
    const record = await env.DB.prepare(
        "SELECT status, reason FROM approval_requests WHERE request_id = ?1",
    ).bind(requestId).first<{ status: ApprovalStatus; reason: DenyReason | "user" | null }>();
    return record?.status === status && record.reason === reason;
}

/** Inserts an immutable audit row after the terminal state was successfully claimed. */
export async function writeTerminalAudit(
    env: Env,
    request: Pick<RequestIndex, "request_id" | "device_id" | "tool" | "summary" | "expires_at">,
    sessionId: string,
    status: Exclude<ApprovalStatus, "pending">,
    reason: DenyReason | "user",
    decidedAt: number,
): Promise<void> {
    const decision = status === "allow" ? "allow" : "deny";
    await env.DB.prepare(
        "INSERT INTO approval_audit " +
        "(request_id, device_id, session_id, tool, summary, decision, reason, expires_at, decided_at) " +
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) ON CONFLICT(request_id) DO NOTHING",
    ).bind(request.request_id, request.device_id, sessionId, request.tool, request.summary,
        decision, reason, request.expires_at, decidedAt).run();
}

export type HookNotifyResult = "sent" | "offline" | "error";

export interface HookNotifyLogRow {
    id: string;
    device_id: string;
    session_id: string | null;
    title: string;
    content: string;
    result: HookNotifyResult;
    online: number;
    created_at: number;
}

export type DeviceEventType = "online" | "offline";

export interface DeviceEventRow {
    id: number;
    device_id: string;
    event: DeviceEventType;
    created_at: number;
}

/**
 * Records a device connection event (heartbeat). Consecutive events of the same
 * type within 60s are deduplicated so reconnect flaps do not flood the table.
 */
export async function writeDeviceEvent(
    env: Env,
    deviceId: string,
    event: DeviceEventType,
    createdAt = Math.floor(Date.now() / 1000),
): Promise<void> {
    const last = await env.DB.prepare(
        "SELECT event, created_at FROM device_events WHERE device_id = ?1 ORDER BY created_at DESC LIMIT 1",
    ).bind(deviceId).first<{ event: DeviceEventType; created_at: number }>();
    if (last && last.event === event && createdAt - last.created_at < 60) return;
    await env.DB.prepare(
        "INSERT INTO device_events (device_id, event, created_at) VALUES (?1, ?2, ?3)",
    ).bind(deviceId, event, createdAt).run();
}

/** Returns connection events for the last `windowSeconds` (newest first). */
export async function listDeviceEvents(
    env: Env,
    opts: { deviceId?: string; since?: number; limit?: number } = {},
): Promise<DeviceEventRow[]> {
    const since = opts.since ?? 0;
    const limit = Math.max(1, Math.min(500, opts.limit ?? 200));
    if (opts.deviceId) {
        const rows = await env.DB.prepare(
            "SELECT id, device_id, event, created_at FROM device_events " +
            "WHERE device_id = ?1 AND created_at >= ?2 ORDER BY created_at DESC LIMIT ?3",
        ).bind(opts.deviceId, since, limit).all<DeviceEventRow>();
        return rows.results;
    }
    const rows = await env.DB.prepare(
        "SELECT id, device_id, event, created_at FROM device_events " +
        "WHERE created_at >= ?1 ORDER BY created_at DESC LIMIT ?2",
    ).bind(since, limit).all<DeviceEventRow>();
    return rows.results;
}

/**
 * Records a hook-notify push to a target device. The Worker calls this after the
 * Durable Object has attempted the push so the admin dashboard can audit what was
 * sent and whether the device was online/delivered.
 */
export async function writeHookNotifyLog(
    env: Env,
    log: Pick<HookNotifyLogRow, "id" | "device_id" | "session_id" | "title" | "content" | "result" | "online" | "created_at">,
): Promise<void> {
    await env.DB.prepare(
        "INSERT INTO hook_notify_log (id, device_id, session_id, title, content, result, online, created_at) " +
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) ON CONFLICT(id) DO NOTHING",
    ).bind(log.id, log.device_id, log.session_id, log.title, log.content, log.result,
        log.online ? 1 : 0, log.created_at).run();
}

/**
 * Query recent hook-notify logs, newest first. `limit` is 1..200 (default 50).
 * Supports an optional `device_id` filter for the admin dashboard.
 */
export async function listHookNotifyLogs(
    env: Env,
    opts: { deviceId?: string; limit?: number } = {},
): Promise<HookNotifyLogRow[]> {
    const limit = Math.max(1, Math.min(200, opts.limit ?? 50));
    if (opts.deviceId) {
        const rows = await env.DB.prepare(
            "SELECT id, device_id, session_id, title, content, result, online, created_at " +
            "FROM hook_notify_log WHERE device_id = ?1 ORDER BY created_at DESC LIMIT ?2",
        ).bind(opts.deviceId, limit).all<HookNotifyLogRow>();
        return rows.results;
    }
    const rows = await env.DB.prepare(
        "SELECT id, device_id, session_id, title, content, result, online, created_at " +
        "FROM hook_notify_log ORDER BY created_at DESC LIMIT ?1",
    ).bind(limit).all<HookNotifyLogRow>();
    return rows.results;
}
