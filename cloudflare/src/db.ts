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

export async function writeTerminalAudit(
    env: Env,
    request: Pick<RequestIndex, "request_id" | "device_id" | "tool" | "summary" | "expires_at">,
    sessionId: string,
    status: Exclude<ApprovalStatus, "pending">,
    reason: DenyReason | "user",
    decidedAt: number,
): Promise<void> {
    const decision = status === "allow" ? "allow" : "deny";
    await env.DB.batch([
        env.DB.prepare(
            "UPDATE approval_requests SET status = ?1, reason = ?2, decided_at = ?3 WHERE request_id = ?4",
        ).bind(status, reason, decidedAt, request.request_id),
        env.DB.prepare(
            "INSERT OR REPLACE INTO approval_audit " +
            "(request_id, device_id, session_id, tool, summary, decision, reason, expires_at, decided_at) " +
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        ).bind(request.request_id, request.device_id, sessionId, request.tool, request.summary,
            decision, reason, request.expires_at, decidedAt),
    ]);
}
