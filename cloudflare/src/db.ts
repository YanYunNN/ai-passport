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
