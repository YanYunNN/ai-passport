import { DurableObject } from "cloudflare:workers";
import { claimTerminalState, forceDenyAfterUncertainAllow, terminalStateMatches, type ApprovalStatus, type DenyReason, writeTerminalAudit } from "./db";
import type { Env } from "./env";
import {
    isDeviceId,
    parseDecision,
    parseHello,
    type DeviceRequest,
    serializeDeviceRequest,
} from "./protocol";

interface PendingRequest {
    requestId: string;
    deviceId: string;
    sessionId: string;
    tool: string;
    summary: string;
    expiresAt: number;
}

interface Outcome {
    requestId: string;
    status: Exclude<ApprovalStatus, "pending">;
    reason: DenyReason | "user";
}

interface Projection {
    pending: PendingRequest;
    outcome: Outcome;
    decidedAt: number;
    /** The D1 terminal claim was already made before this projection is audited. */
    claimed?: boolean;
    /** Retry a conditional compensation when an allow D1 response was uncertain. */
    forceDeny?: boolean;
    /** Present only for a user allow from a revalidated socket. */
    credentialHash?: string;
}

interface RelayState {
    deviceId?: string;
    currentSessionId?: string;
    pending?: PendingRequest;
    outbox?: Projection[];
}

interface SocketAttachment {
    deviceId: string;
    credentialHash: string;
    sessionId?: string;
}

interface DeviceAuthorizationRecord {
    credential_hash: string;
    previous_credential_hash: string | null;
    previous_credential_expires_at: number | null;
    status: "active" | "revoked";
}

const STATE_KEY = "relay_state";
const requestKey = (requestId: string): string => `request:${requestId}`;
const nowSeconds = (): number => Math.floor(Date.now() / 1000);
const projectionRetryDelayMs = 10_000;

export class PassportRelay extends DurableObject<Env> {
    private readonly sockets = new Map<WebSocket, SocketAttachment>();

    constructor(ctx: DurableObjectState, env: Env) {
        super(ctx, env);
        for (const socket of ctx.getWebSockets()) {
            const attachment = socket.deserializeAttachment() as SocketAttachment | null;
            if (attachment && isDeviceId(attachment.deviceId) && attachment.credentialHash) {
                this.sockets.set(socket, attachment);
            }
        }
    }

    async fetch(request: Request): Promise<Response> {
        const url = new URL(request.url);
        if (request.method === "GET" && url.pathname.startsWith("/device/")) {
            return this.connectDevice(request, url.pathname.slice("/device/".length));
        }
        if (request.method === "POST" && url.pathname === "/internal/requests") return this.createRequest(request);
        if (request.method === "GET" && url.pathname.startsWith("/internal/requests/")) {
            return this.getRequest(request, url.pathname.slice("/internal/requests/".length));
        }
        if (request.method === "POST" && url.pathname === "/internal/revoke") return this.revoke(request);
        if (request.method === "GET" && url.pathname === "/internal/status") {
            const openSockets = this.ctx.getWebSockets();
            return json({ online: openSockets.length > 0 || this.sockets.size > 0 });
        }
        return json({ error: "not found" }, 404);
    }

    async alarm(): Promise<void> {
        const state = await this.loadState();
        if (state.pending && state.pending.expiresAt <= nowSeconds()) {
            await this.finishPending(state.pending, "deny", "timeout");
            return;
        }
        await this.flushOutbox();
    }

    async webSocketMessage(socket: WebSocket, message: ArrayBuffer | string): Promise<void> {
        const attachment = this.attachmentFor(socket);
        if (!attachment || typeof message !== "string") {
            await this.denyCurrentForProtocolError();
            return;
        }
        if (!(await this.credentialStillValid(attachment))) {
            await this.invalidateSession(attachment.sessionId, true);
            return;
        }
        if (!attachment.sessionId) {
            const hello = parseHello(message, attachment.deviceId);
            if (!hello) {
                socket.close(1008, "invalid hello");
                return;
            }
            await this.handleHello(socket, attachment, hello.session_id);
            return;
        }

        const state = await this.loadState();
        const decision = parseDecision(message, attachment.deviceId);
        if (!decision || state.deviceId !== attachment.deviceId || state.currentSessionId !== attachment.sessionId ||
            decision.session_id !== attachment.sessionId || !state.pending ||
            decision.request_id !== state.pending.requestId || decision.session_id !== state.pending.sessionId ||
            state.pending.expiresAt <= nowSeconds()) {
            await this.denyCurrentForProtocolError();
            return;
        }
        await this.finishPending(state.pending, decision.decision, decision.reason, attachment.credentialHash);
        socket.send(`{"v":1,"type":"decision_ack","request_id":"${decision.request_id}",` +
            `"session_id":"${decision.session_id}","accepted":true}`);
    }

    async webSocketClose(socket: WebSocket): Promise<void> {
        const attachment = this.attachmentFor(socket);
        this.sockets.delete(socket);
        await this.invalidateSession(attachment?.sessionId, false);
    }

    async webSocketError(socket: WebSocket): Promise<void> {
        await this.webSocketClose(socket);
    }

    private async connectDevice(request: Request, deviceId: string): Promise<Response> {
        const credentialHash = request.headers.get("X-Passport-Credential-Hash");
        if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket" || !isDeviceId(deviceId) ||
            request.headers.get("X-Passport-Authenticated-Device") !== deviceId || !credentialHash) {
            return json({ error: "unauthorized" }, 401);
        }
        const state = await this.loadState();
        if (state.deviceId && state.deviceId !== deviceId) return json({ error: "device mismatch" }, 403);
        if (!state.deviceId) await this.saveState({ ...state, deviceId });

        const pair = new WebSocketPair();
        const [client, server] = Object.values(pair) as [WebSocket, WebSocket];
        const attachment: SocketAttachment = { deviceId, credentialHash };
        server.serializeAttachment(attachment);
        this.ctx.acceptWebSocket(server);
        this.sockets.set(server, attachment);
        return new Response(null, { status: 101, webSocket: client });
    }

    private async handleHello(socket: WebSocket, attachment: SocketAttachment, sessionId: string): Promise<void> {
        const state = await this.loadState();
        if (state.deviceId && state.deviceId !== attachment.deviceId) {
            socket.close(1008, "device mismatch");
            return;
        }
        if (state.currentSessionId) await this.invalidateSession(state.currentSessionId, true);
        const nextAttachment: SocketAttachment = { ...attachment, sessionId };
        socket.serializeAttachment(nextAttachment);
        this.sockets.set(socket, nextAttachment);
        const current = await this.loadState();
        await this.saveState({ ...current, deviceId: attachment.deviceId, currentSessionId: sessionId });
    }

    private async createRequest(request: Request): Promise<Response> {
        const deviceId = request.headers.get("X-Passport-Device-Id");
        if (!deviceId || !isDeviceId(deviceId)) return json({ error: "unauthorized" }, 401);
        const input = await request.json<Partial<PendingRequest>>().catch(() => null);
        if (!input || input.deviceId !== deviceId || typeof input.requestId !== "string" ||
            typeof input.tool !== "string" || typeof input.summary !== "string" || typeof input.expiresAt !== "number") {
            return json({ error: "invalid internal request" }, 400);
        }
        const state = await this.loadState();
        if (state.pending) return json({ error: "approval already pending" }, 409);
        const socket = state.currentSessionId ? await this.currentAuthorizedSocket(state.currentSessionId) : undefined;
        const created: PendingRequest = {
            requestId: input.requestId,
            deviceId,
            sessionId: state.currentSessionId ?? "offline",
            tool: input.tool,
            summary: input.summary,
            expiresAt: input.expiresAt,
        };
        if (created.expiresAt <= nowSeconds() || !socket) {
            const outcome: Outcome = { requestId: created.requestId, status: "deny", reason: "offline" };
            await this.storeOutcome(created, outcome);
            return json(toResponse(outcome));
        }

        await this.saveState({ ...state, pending: created });
        await this.ctx.storage.put(requestKey(created.requestId), { status: "pending" });
        await this.refreshAlarm(await this.loadState());
        try {
            socket.send(serializeDeviceRequest({
                v: 1,
                type: "request",
                device_id: created.deviceId,
                session_id: created.sessionId,
                request_id: created.requestId,
                tool: created.tool,
                summary: created.summary,
                expires_at: created.expiresAt,
            }));
        } catch {
            await this.finishPending(created, "deny", "offline");
        }
        return this.getStoredResponse(created.requestId);
    }

    private async getRequest(request: Request, requestId: string): Promise<Response> {
        const deviceId = request.headers.get("X-Passport-Device-Id");
        if (!deviceId || !isDeviceId(deviceId)) return json({ error: "unauthorized" }, 401);
        const state = await this.loadState();
        if (state.deviceId !== deviceId) return json({ error: "not found" }, 404);
        return this.getStoredResponse(requestId);
    }

    private async revoke(request: Request): Promise<Response> {
        const deviceId = request.headers.get("X-Passport-Device-Id");
        if (!deviceId || !isDeviceId(deviceId)) return json({ error: "unauthorized" }, 401);
        const state = await this.loadState();
        if (state.currentSessionId) await this.invalidateSession(state.currentSessionId, true);
        return json({ status: "revoked" });
    }

    private async getStoredResponse(requestId: string): Promise<Response> {
        const stored = await this.ctx.storage.get<{ status: ApprovalStatus; reason?: DenyReason | "user" }>(requestKey(requestId));
        if (!stored) return json({ error: "not found" }, 404);
        return json(stored.status === "pending" ? { status: "pending" } : {
            status: stored.status,
            reason: stored.reason,
        });
    }

    private attachmentFor(socket: WebSocket): SocketAttachment | null {
        return this.sockets.get(socket) ?? socket.deserializeAttachment() as SocketAttachment | null;
    }

    private currentSocket(sessionId: string): WebSocket | undefined {
        for (const [socket, attachment] of this.sockets) {
            if (attachment.sessionId === sessionId) return socket;
        }
        return undefined;
    }

    private async currentAuthorizedSocket(sessionId: string): Promise<WebSocket | undefined> {
        const socket = this.currentSocket(sessionId);
        const attachment = socket ? this.attachmentFor(socket) : null;
        if (socket && attachment && await this.credentialStillValid(attachment)) return socket;
        await this.invalidateSession(sessionId, true);
        return undefined;
    }

    private async credentialStillValid(attachment: SocketAttachment): Promise<boolean> {
        const record = await this.env.DB.prepare(
            "SELECT credential_hash, previous_credential_hash, previous_credential_expires_at, status " +
            "FROM devices WHERE device_id = ?1",
        ).bind(attachment.deviceId).first<DeviceAuthorizationRecord>();
        if (!record || record.status !== "active") return false;
        if (record.credential_hash === attachment.credentialHash) return true;
        return record.previous_credential_hash === attachment.credentialHash &&
            record.previous_credential_expires_at !== null && record.previous_credential_expires_at >= nowSeconds();
    }

    private async invalidateSession(sessionId: string | undefined, closeSockets: boolean): Promise<void> {
        if (!sessionId) return;
        const state = await this.loadState();
        if (closeSockets) {
            for (const [socket, attachment] of this.sockets) {
                if (attachment.sessionId === sessionId) {
                    socket.close(1008, "session invalidated");
                    this.sockets.delete(socket);
                }
            }
        }
        if (state.currentSessionId !== sessionId) return;
        await this.saveState({ ...state, currentSessionId: undefined });
        if (state.pending?.sessionId === sessionId) await this.finishPending(state.pending, "deny", "session_lost");
    }

    private async denyCurrentForProtocolError(): Promise<void> {
        const state = await this.loadState();
        if (state.pending) await this.finishPending(state.pending, "deny", "protocol_error");
    }

    private async finishPending(
        pending: PendingRequest,
        status: Exclude<ApprovalStatus, "pending">,
        reason: DenyReason | "user",
        credentialHash?: string,
    ): Promise<void> {
        const state = await this.loadState();
        if (!state.pending || state.pending.requestId !== pending.requestId) return;

        const decidedAt = nowSeconds();
        let outcome: Outcome = { requestId: pending.requestId, status, reason };
        let claimed = false;
        let forceDeny = false;
        if (status === "allow" && credentialHash) {
            try {
                // Claim D1 before publishing an allow in DO storage. The conditional SQL
                // rechecks pending status, expiry, device state, and this socket's credential.
                claimed = await claimTerminalState(this.env, requestIndex(pending), "allow", reason, decidedAt, credentialHash);
            } catch (error) {
                // The D1 operation may have committed even though this execution did not
                // receive a result; persist a conditional deny compensation for retry.
                forceDeny = true;
                console.error("Unable to claim Passport allow in D1", error);
            }
        }
        if (status === "allow" && !claimed) {
            // A lost race, invalid/revoked credential, expiry, or D1 failure must never allow.
            outcome = { requestId: pending.requestId, status: "deny", reason: "policy" };
        }

        const projection: Projection = { pending, outcome, decidedAt, claimed, forceDeny, credentialHash };
        const next: RelayState = { ...state, pending: undefined, outbox: [...(state.outbox ?? []), projection] };
        await this.ctx.storage.put({ [STATE_KEY]: next, [requestKey(pending.requestId)]: projection.outcome });
        await this.flushOutbox();
    }

    private async storeOutcome(pending: PendingRequest, outcome: Outcome): Promise<void> {
        const state = await this.loadState();
        const projection: Projection = { pending, outcome, decidedAt: nowSeconds() };
        const next: RelayState = { ...state, outbox: [...(state.outbox ?? []), projection] };
        await this.ctx.storage.put({ [STATE_KEY]: next, [requestKey(pending.requestId)]: outcome });
        await this.flushOutbox();
    }

    private async flushOutbox(): Promise<void> {
        let state = await this.loadState();
        const remaining: Projection[] = [];
        for (const projection of state.outbox ?? []) {
            try {
                if (!projection.claimed) {
                    const claimed = await claimTerminalState(
                        this.env,
                        requestIndex(projection.pending),
                        projection.outcome.status,
                        projection.outcome.reason,
                        projection.decidedAt,
                        projection.credentialHash,
                    );
                    if (!claimed) {
                        if (projection.forceDeny) {
                            const denied = await forceDenyAfterUncertainAllow(
                                this.env,
                                requestIndex(projection.pending),
                                projection.decidedAt,
                            );
                            if (!denied && !await terminalStateMatches(
                                this.env,
                                projection.pending.requestId,
                                "deny",
                                "policy",
                            )) continue;
                        } else if (!await terminalStateMatches(
                            this.env,
                            projection.pending.requestId,
                            projection.outcome.status,
                            projection.outcome.reason,
                        )) {
                            // A non-pending/unknown request was handled elsewhere; never
                            // create an audit record that conflicts with D1's terminal state.
                            continue;
                        }
                    }
                    projection.claimed = true;
                }
                await writeTerminalAudit(this.env, requestIndex(projection.pending), projection.pending.sessionId,
                    projection.outcome.status, projection.outcome.reason, projection.decidedAt);
            } catch (error) {
                console.error("Unable to project Passport terminal state to D1", error);
                remaining.push(projection);
            }
        }
        state = await this.loadState();
        await this.saveState({ ...state, outbox: remaining });
        await this.refreshAlarm(await this.loadState());
    }

    private async loadState(): Promise<RelayState> {
        return await this.ctx.storage.get<RelayState>(STATE_KEY) ?? {};
    }

    private async saveState(state: RelayState): Promise<void> {
        await this.ctx.storage.put(STATE_KEY, state);
    }

    private async refreshAlarm(state: RelayState): Promise<void> {
        const alarmAt = Math.min(
            state.pending ? state.pending.expiresAt * 1000 : Number.POSITIVE_INFINITY,
            state.outbox?.length ? Date.now() + projectionRetryDelayMs : Number.POSITIVE_INFINITY,
        );
        if (Number.isFinite(alarmAt)) await this.ctx.storage.setAlarm(alarmAt);
        else await this.ctx.storage.deleteAlarm();
    }
}

function requestIndex(pending: PendingRequest): {
    request_id: string;
    device_id: string;
    tool: string;
    summary: string;
    expires_at: number;
} {
    return {
        request_id: pending.requestId,
        device_id: pending.deviceId,
        tool: pending.tool,
        summary: pending.summary,
        expires_at: pending.expiresAt,
    };
}

function toResponse(outcome: Outcome): { status: "allow" | "deny"; reason: DenyReason | "user" } {
    return { status: outcome.status, reason: outcome.reason };
}

function json(body: unknown, status = 200): Response {
    return new Response(JSON.stringify(body), {
        status,
        headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" },
    });
}
