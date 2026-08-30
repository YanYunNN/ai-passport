import { DurableObject } from "cloudflare:workers";
import { claimTerminalState, forceDenyAfterUncertainAllow, terminalStateMatches, type ApprovalStatus, type DenyReason, writeTerminalAudit } from "./db";
import type { Env } from "./env";
import {
    isDeviceId,
    parseBinaryScreencastMessage,
    parseDecision,
    parseHello,
    parseScreencastMessage,
    type DeviceRequest,
    serializeDeviceNotify,
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
    credentialValidUntil?: number;
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
// ponytail: cache authorization for at most 5s to avoid one D1 query per video slice;
// explicit revocation still closes the socket immediately. Lower this TTL if revocation bypasses the DO.
const credentialCacheMs = 5_000;

export class PassportRelay extends DurableObject<Env> {
    private readonly sockets = new Map<WebSocket, SocketAttachment>();
    private readonly adminViewers = new Set<WebSocket>();
    private readonly latestSlices = new Map<number, string>();
    private latestFrameSeq?: number;

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
        if (request.method === "POST" && url.pathname === "/internal/notify") return this.pushNotify(request);
        if (request.method === "GET" && url.pathname.startsWith("/internal/requests/")) {
            return this.getRequest(request, url.pathname.slice("/internal/requests/".length));
        }
        if (request.method === "POST" && url.pathname === "/internal/revoke") return this.revoke(request);
        if (request.method === "POST" && url.pathname === "/internal/send-image") return this.sendImage(request);
        if (request.method === "GET" && url.pathname === "/internal/screencast/ws") {
            return this.connectAdminViewer(request);
        }
        if (request.method === "POST" && url.pathname === "/internal/screencast/command") {
            return this.sendScreencastCommand(request);
        }
        if (request.method === "GET" && url.pathname === "/internal/screencast/latest") {
            const slices = Array.from(this.latestSlices.values()).map((s) => JSON.parse(s));
            return json({ slices });
        }
        if (request.method === "GET" && url.pathname === "/internal/status") {
            const openSockets = this.ctx.getWebSockets();
            return json({ online: openSockets.length > 0 || this.sockets.size > 0 });
        }
        return json({ error: "not found" }, 404);
    }

    private connectAdminViewer(request: Request): Response {
        if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
            return json({ error: "expected websocket upgrade" }, 400);
        }
        const pair = new WebSocketPair();
        const [client, server] = Object.values(pair) as [WebSocket, WebSocket];
        this.ctx.acceptWebSocket(server);
        this.adminViewers.add(server);
        return new Response(null, { status: 101, webSocket: client });
    }

    private async sendScreencastCommand(request: Request): Promise<Response> {
        const payload = await request.text();
        const sockets = this.ctx.getWebSockets();
        const allSockets = new Set([...sockets, ...this.sockets.keys()]);
        let sent = false;
        for (const socket of allSockets) {
            if (!this.adminViewers.has(socket)) {
                try {
                    socket.send(payload);
                    sent = true;
                } catch {}
            }
        }
        return json({ sent, online: allSockets.size > 0 });
    }

    private async sendImage(request: Request): Promise<Response> {
        const payload = await request.json<{ imageId: string; title?: string; data: string }>().catch(() => null);
        if (!payload || typeof payload.data !== "string" || typeof payload.imageId !== "string") {
            return json({ error: "invalid image payload" }, 400);
        }
        const sockets = this.ctx.getWebSockets();
        const allSockets = new Set([...sockets, ...this.sockets.keys()]);
        if (allSockets.size === 0) {
            return json({ sent: false, online: false });
        }

        const CHUNK_SIZE = 768; // must be multiple of 4 for valid base64
        const totalChunks = Math.ceil(payload.data.length / CHUNK_SIZE);

        for (const socket of allSockets) {
            if (!this.adminViewers.has(socket)) {
                try {
                    socket.send(JSON.stringify({
                        v: 1,
                        type: "image_start",
                        id: payload.imageId,
                        title: payload.title || "Image",
                        chunks: totalChunks,
                    }));

                    for (let i = 0; i < totalChunks; i++) {
                        const chunk = payload.data.slice(i * CHUNK_SIZE, (i + 1) * CHUNK_SIZE);
                        socket.send(JSON.stringify({
                            v: 1,
                            type: "image_chunk",
                            id: payload.imageId,
                            seq: i,
                            data: chunk,
                        }));
                    }

                    socket.send(JSON.stringify({
                        v: 1,
                        type: "image_end",
                        id: payload.imageId,
                    }));
                } catch (err) {
                    console.error("sendImage error", err);
                }
            }
        }
        return json({ sent: true, online: true });
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
        if (this.adminViewers.has(socket)) {
            if (typeof message === "string") {
                const sockets = this.ctx.getWebSockets();
                const allSockets = new Set([...sockets, ...this.sockets.keys()]);
                for (const s of allSockets) {
                    if (!this.adminViewers.has(s)) {
                        try { s.send(message); } catch {}
                    }
                }
            }
            return;
        }

        const attachment = this.attachmentFor(socket);
        if (!attachment) {
            await this.denyCurrentForProtocolError();
            return;
        }
        if (!(await this.credentialStillValid(attachment))) {
            await this.invalidateSession(attachment.sessionId, true);
            return;
        }
        if (!attachment.sessionId) {
            if (typeof message !== "string") {
                socket.close(1008, "invalid hello");
                return;
            }
            const hello = parseHello(message, attachment.deviceId);
            if (!hello) {
                socket.close(1008, "invalid hello");
                return;
            }
            await this.handleHello(socket, attachment, hello.session_id);
            return;
        }

        if (typeof message !== "string" || message.includes('"screencast"')) {
            const screencast = typeof message === "string"
                ? parseScreencastMessage(message)
                : parseBinaryScreencastMessage(message);
            if (!screencast) {
                await this.denyCurrentForProtocolError();
                return;
            }
            const viewerMessage = typeof message === "string" ? message : JSON.stringify(screencast);
            if (this.latestFrameSeq !== screencast.seq) {
                this.latestSlices.clear();
                this.latestFrameSeq = screencast.seq;
            }
            this.latestSlices.set(screencast.slice, viewerMessage);
            for (const viewer of this.adminViewers) {
                try { viewer.send(viewerMessage); } catch {}
            }
            socket.send(`{"v":1,"type":"screencast_ack","seq":${screencast.seq},` +
                `"slice":${screencast.slice}}`);
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
        if (this.adminViewers.has(socket)) {
            this.adminViewers.delete(socket);
            return;
        }
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
        const attachment: SocketAttachment = {
            deviceId,
            credentialHash,
            credentialValidUntil: Date.now() + credentialCacheMs,
        };
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
        this.latestSlices.clear();
        this.latestFrameSeq = undefined;
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

    private async pushNotify(request: Request): Promise<Response> {
        const deviceId = request.headers.get("X-Passport-Device-Id");
        if (!deviceId || !isDeviceId(deviceId)) return json({ error: "unauthorized" }, 401);
        const input = await request.json<{ title?: string; content?: string; ts?: number; id?: string }>().catch(() => null);
        if (!input || typeof input.content !== "string" || typeof input.id !== "string" ||
            typeof input.ts !== "number" || !Number.isSafeInteger(input.ts)) {
            return json({ error: "invalid notify body" }, 400);
        }
        const state = await this.loadState();
        if (!state.currentSessionId || !state.deviceId) return json({ sent: false, online: false });
        if (state.deviceId !== deviceId) return json({ error: "device mismatch" }, 403);
        const socket = await this.currentAuthorizedSocket(state.currentSessionId);
        if (!socket) return json({ sent: false, online: false });

        const payload = serializeDeviceNotify({
            device_id: deviceId,
            session_id: state.currentSessionId,
            id: input.id,
            title: input.title ?? "Agent",
            content: input.content,
            ts: input.ts,
        });
        try {
            socket.send(payload);
        } catch (error) {
            console.error("pushNotify send error", error);
            return json({ sent: false, online: false });
        }
        return json({ sent: true, online: true });
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
        if ((attachment.credentialValidUntil ?? 0) > Date.now()) return true;

        const record = await this.env.DB.prepare(
            "SELECT credential_hash, previous_credential_hash, previous_credential_expires_at, status " +
            "FROM devices WHERE device_id = ?1",
        ).bind(attachment.deviceId).first<DeviceAuthorizationRecord>();
        if (!record || record.status !== "active") return false;

        const valid = record.credential_hash === attachment.credentialHash ||
            (record.previous_credential_hash === attachment.credentialHash &&
                record.previous_credential_expires_at !== null &&
                record.previous_credential_expires_at >= nowSeconds());
        if (valid) attachment.credentialValidUntil = Date.now() + credentialCacheMs;
        return valid;
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
