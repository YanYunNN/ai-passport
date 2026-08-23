import { DurableObject } from "cloudflare:workers";
import { type ApprovalStatus, type DenyReason, writeTerminalAudit } from "./db";
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

interface RelayState {
    deviceId?: string;
    currentSessionId?: string;
    pending?: PendingRequest;
}

interface SocketAttachment {
    deviceId: string;
    sessionId?: string;
}

const STATE_KEY = "relay_state";
const requestKey = (requestId: string): string => `request:${requestId}`;
const nowSeconds = (): number => Math.floor(Date.now() / 1000);

export class PassportRelay extends DurableObject<Env> {
    private readonly sockets = new Map<WebSocket, SocketAttachment>();

    constructor(ctx: DurableObjectState, env: Env) {
        super(ctx, env);
        for (const socket of ctx.getWebSockets()) {
            const attachment = socket.deserializeAttachment() as SocketAttachment | null;
            if (attachment && isDeviceId(attachment.deviceId)) this.sockets.set(socket, attachment);
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
        return json({ error: "not found" }, 404);
    }

    async alarm(): Promise<void> {
        const state = await this.loadState();
        if (!state.pending) return;
        if (state.pending.expiresAt <= nowSeconds()) {
            await this.finishPending(state.pending, "deny", "timeout");
        } else {
            await this.ctx.storage.setAlarm(state.pending.expiresAt * 1000);
        }
    }

    async webSocketMessage(socket: WebSocket, message: ArrayBuffer | string): Promise<void> {
        const attachment = this.sockets.get(socket) ?? socket.deserializeAttachment() as SocketAttachment | null;
        if (!attachment || typeof message !== "string") {
            await this.denyCurrentForProtocolError();
            return;
        }
        const state = await this.loadState();
        if (!attachment.sessionId) {
            const hello = parseHello(message, attachment.deviceId);
            if (!hello) {
                socket.close(1008, "invalid hello");
                return;
            }
            await this.handleHello(socket, attachment, hello.session_id);
            return;
        }

        const decision = parseDecision(message, attachment.deviceId);
        if (!decision || state.deviceId !== attachment.deviceId || state.currentSessionId !== attachment.sessionId ||
            decision.session_id !== attachment.sessionId || !state.pending ||
            decision.request_id !== state.pending.requestId || decision.session_id !== state.pending.sessionId ||
            state.pending.expiresAt <= nowSeconds()) {
            await this.denyCurrentForProtocolError();
            return;
        }
        await this.finishPending(state.pending, decision.decision, decision.reason);
        socket.send(`{"v":1,"type":"decision_ack","request_id":"${decision.request_id}",` +
            `"session_id":"${decision.session_id}","accepted":true}`);
    }

    async webSocketClose(socket: WebSocket): Promise<void> {
        const attachment = this.sockets.get(socket) ?? socket.deserializeAttachment() as SocketAttachment | null;
        this.sockets.delete(socket);
        if (!attachment?.sessionId) return;
        const state = await this.loadState();
        if (state.currentSessionId === attachment.sessionId) {
            const next: RelayState = { ...state, currentSessionId: undefined };
            await this.saveState(next);
            if (state.pending?.sessionId === attachment.sessionId) {
                await this.finishPending(state.pending, "deny", "session_lost");
            }
        }
    }

    async webSocketError(socket: WebSocket): Promise<void> {
        await this.webSocketClose(socket);
    }

    private async connectDevice(request: Request, deviceId: string): Promise<Response> {
        if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket" || !isDeviceId(deviceId) ||
            request.headers.get("X-Passport-Authenticated-Device") !== deviceId) {
            return json({ error: "unauthorized" }, 401);
        }
        const state = await this.loadState();
        if (state.deviceId && state.deviceId !== deviceId) return json({ error: "device mismatch" }, 403);
        if (!state.deviceId) await this.saveState({ ...state, deviceId });

        const pair = new WebSocketPair();
        const [client, server] = Object.values(pair) as [WebSocket, WebSocket];
        const attachment: SocketAttachment = { deviceId };
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
        if (state.pending) await this.finishPending(state.pending, "deny", "session_lost");
        for (const [existing, existingAttachment] of this.sockets) {
            if (existing !== socket && existingAttachment.sessionId) {
                existing.close(1000, "superseded by new session");
                this.sockets.delete(existing);
            }
        }
        const nextAttachment: SocketAttachment = { deviceId: attachment.deviceId, sessionId };
        socket.serializeAttachment(nextAttachment);
        this.sockets.set(socket, nextAttachment);
        await this.saveState({ deviceId: attachment.deviceId, currentSessionId: sessionId });
    }

    private async createRequest(request: Request): Promise<Response> {
        const deviceId = request.headers.get("X-Passport-Device-Id");
        if (!deviceId || !isDeviceId(deviceId)) return json({ error: "unauthorized" }, 401);
        const input = await request.json<Partial<PendingRequest>>().catch(() => null);
        if (!input || input.deviceId !== deviceId || typeof input.requestId !== "string" ||
            typeof input.tool !== "string" || typeof input.summary !== "string" ||
            typeof input.expiresAt !== "number") {
            return json({ error: "invalid internal request" }, 400);
        }
        const state = await this.loadState();
        if (state.pending) return json({ error: "approval already pending" }, 409);
        const created: PendingRequest = {
            requestId: input.requestId,
            deviceId,
            sessionId: state.currentSessionId ?? "offline",
            tool: input.tool,
            summary: input.summary,
            expiresAt: input.expiresAt,
        };
        if (created.expiresAt <= nowSeconds() || !state.currentSessionId ||
            !this.currentSocket(state.currentSessionId)) {
            const outcome: Outcome = { requestId: created.requestId, status: "deny", reason: "offline" };
            await this.storeOutcome(created, outcome);
            return json(toResponse(outcome));
        }

        await this.saveState({ ...state, pending: created });
        await this.ctx.storage.put(requestKey(created.requestId), { status: "pending" });
        await this.ctx.storage.setAlarm(created.expiresAt * 1000);
        try {
            this.currentSocket(created.sessionId)?.send(serializeDeviceRequest({
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

    private async getStoredResponse(requestId: string): Promise<Response> {
        const stored = await this.ctx.storage.get<{ status: ApprovalStatus; reason?: DenyReason | "user" }>(requestKey(requestId));
        if (!stored) return json({ error: "not found" }, 404);
        return json(stored.status === "pending" ? { status: "pending" } : {
            status: stored.status,
            reason: stored.reason,
        });
    }

    private currentSocket(sessionId: string): WebSocket | undefined {
        for (const [socket, attachment] of this.sockets) {
            if (attachment.sessionId === sessionId) return socket;
        }
        return undefined;
    }

    private async denyCurrentForProtocolError(): Promise<void> {
        const state = await this.loadState();
        if (state.pending) await this.finishPending(state.pending, "deny", "protocol_error");
    }

    private async finishPending(
        pending: PendingRequest,
        status: Exclude<ApprovalStatus, "pending">,
        reason: DenyReason | "user",
    ): Promise<void> {
        const state = await this.loadState();
        if (!state.pending || state.pending.requestId !== pending.requestId) return;
        const next: RelayState = { ...state, pending: undefined };
        const outcome: Outcome = { requestId: pending.requestId, status, reason };
        await this.ctx.storage.put({ [STATE_KEY]: next, [requestKey(pending.requestId)]: outcome });
        await this.refreshAlarm(next);
        await writeTerminalAudit(this.env, {
            request_id: pending.requestId,
            device_id: pending.deviceId,
            tool: pending.tool,
            summary: pending.summary,
            expires_at: pending.expiresAt,
        }, pending.sessionId, status, reason, nowSeconds());
    }

    private async storeOutcome(pending: PendingRequest, outcome: Outcome): Promise<void> {
        await this.ctx.storage.put(requestKey(pending.requestId), outcome);
        await writeTerminalAudit(this.env, {
            request_id: pending.requestId,
            device_id: pending.deviceId,
            tool: pending.tool,
            summary: pending.summary,
            expires_at: pending.expiresAt,
        }, pending.sessionId, outcome.status, outcome.reason, nowSeconds());
    }

    private async loadState(): Promise<RelayState> {
        return await this.ctx.storage.get<RelayState>(STATE_KEY) ?? {};
    }

    private async saveState(state: RelayState): Promise<void> {
        await this.ctx.storage.put(STATE_KEY, state);
    }

    private async refreshAlarm(state: RelayState): Promise<void> {
        if (state.pending) await this.ctx.storage.setAlarm(state.pending.expiresAt * 1000);
        else await this.ctx.storage.deleteAlarm();
    }
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
