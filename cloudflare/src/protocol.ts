export const DEVICE_ID_PATTERN = /^passport-[A-F0-9]{12}$/;
export const SESSION_ID_PATTERN = /^[a-f0-9]{32}$/;
export const REQUEST_ID_PATTERN = /^[0-9a-f-]{1,36}$/;
export const TOOL_PATTERN = /^[A-Za-z0-9._:-]{1,31}$/;
export const REASON_PATTERN = /^(user|policy)$/;

const MAX_MESSAGE_BYTES = 511;

export interface HelloMessage {
    v: 1;
    type: "hello";
    device_id: string;
    session_id: string;
}

export interface DecisionMessage {
    v: 1;
    type: "decision";
    device_id: string;
    session_id: string;
    request_id: string;
    decision: "allow" | "deny";
    reason: "user" | "policy";
}

export interface ApprovalInput {
    tool: string;
    summary: string;
    ttlSeconds: number;
}

export interface DeviceRequest {
    v: 1;
    type: "request";
    device_id: string;
    session_id: string;
    request_id: string;
    tool: string;
    summary: string;
    expires_at: number;
}

type FlatValue = string | number;

/**
 * The firmware intentionally accepts only unescaped printable ASCII strings.
 * This parser applies the same narrow grammar to inbound device messages and
 * rejects duplicate/unknown fields before a decision can ever become allow.
 */
export function parseStrictObject(payload: string): Record<string, FlatValue> | null {
    if (!payload || new TextEncoder().encode(payload).byteLength > MAX_MESSAGE_BYTES) return null;

    let index = 0;
    const object: Record<string, FlatValue> = {};
    const skipSpace = (): void => {
        while (index < payload.length && /[ \t\r\n]/.test(payload[index])) index++;
    };
    const readString = (): string | null => {
        skipSpace();
        if (payload[index++] !== '"') return null;
        let value = "";
        while (index < payload.length && payload[index] !== '"') {
            const code = payload.charCodeAt(index);
            if (code < 0x20 || code > 0x7e || payload[index] === "\\") return null;
            value += payload[index++];
        }
        if (payload[index++] !== '"' || !value) return null;
        return value;
    };
    const readNumber = (): number | null => {
        skipSpace();
        const start = index;
        while (index < payload.length && /[0-9]/.test(payload[index])) index++;
        if (start === index) return null;
        const raw = payload.slice(start, index);
        if (raw.length > 1 && raw.startsWith("0")) return null;
        const value = Number(raw);
        return Number.isSafeInteger(value) ? value : null;
    };

    skipSpace();
    if (payload[index++] !== "{") return null;
    skipSpace();
    if (payload[index] === "}") return null;
    while (index < payload.length) {
        const key = readString();
        if (!key || Object.hasOwn(object, key)) return null;
        skipSpace();
        if (payload[index++] !== ":") return null;
        skipSpace();
        const value = payload[index] === '"' ? readString() : readNumber();
        if (value === null) return null;
        object[key] = value;
        skipSpace();
        if (payload[index] === "}") {
            index++;
            break;
        }
        if (payload[index++] !== ",") return null;
    }
    skipSpace();
    return index === payload.length ? object : null;
}

function hasExactly(object: Record<string, FlatValue>, keys: readonly string[]): boolean {
    return Object.keys(object).length === keys.length && keys.every((key) => Object.hasOwn(object, key));
}

export function parseHello(payload: string, expectedDeviceId: string): HelloMessage | null {
    const object = parseStrictObject(payload);
    if (!object || !hasExactly(object, ["v", "type", "device_id", "session_id"])) return null;
    if (object.v !== 1 || object.type !== "hello" || object.device_id !== expectedDeviceId ||
        typeof object.session_id !== "string" || !SESSION_ID_PATTERN.test(object.session_id)) return null;
    return object as unknown as HelloMessage;
}

export function parseDecision(payload: string, expectedDeviceId: string): DecisionMessage | null {
    const object = parseStrictObject(payload);
    const keys = ["v", "type", "device_id", "session_id", "request_id", "decision", "reason"];
    if (!object || !hasExactly(object, keys)) return null;
    if (object.v !== 1 || object.type !== "decision" || object.device_id !== expectedDeviceId ||
        typeof object.session_id !== "string" || typeof object.request_id !== "string" ||
        typeof object.decision !== "string" || typeof object.reason !== "string" ||
        !SESSION_ID_PATTERN.test(object.session_id) || !REQUEST_ID_PATTERN.test(object.request_id) ||
        (object.decision !== "allow" && object.decision !== "deny") || !REASON_PATTERN.test(object.reason)) {
        return null;
    }
    return object as unknown as DecisionMessage;
}

function printableAscii(value: unknown, maxLength: number): value is string {
    return typeof value === "string" && value.length > 0 && value.length <= maxLength &&
        /^[ -!#-\[\]-~]+$/.test(value);
}

export function parseApprovalInput(value: unknown): ApprovalInput | null {
    if (!value || typeof value !== "object" || Array.isArray(value)) return null;
    const object = value as Record<string, unknown>;
    if (Object.keys(object).length !== 3 || !Object.hasOwn(object, "tool") ||
        !Object.hasOwn(object, "summary") || !Object.hasOwn(object, "ttl_seconds")) return null;
    if (!printableAscii(object.tool, 31) || !TOOL_PATTERN.test(object.tool) ||
        !printableAscii(object.summary, 71) || typeof object.ttl_seconds !== "number" ||
        !Number.isInteger(object.ttl_seconds) || object.ttl_seconds < 1 || object.ttl_seconds > 300) {
        return null;
    }
    return { tool: object.tool, summary: object.summary, ttlSeconds: object.ttl_seconds };
}

/** Build a request that needs no JSON escaping, matching the firmware's parser. */
export function serializeDeviceRequest(request: DeviceRequest): string {
    const payload = `{"v":1,"type":"request","device_id":"${request.device_id}",` +
        `"session_id":"${request.session_id}","request_id":"${request.request_id}",` +
        `"tool":"${request.tool}","summary":"${request.summary}","expires_at":${request.expires_at}}`;
    if (new TextEncoder().encode(payload).byteLength > MAX_MESSAGE_BYTES) {
        throw new Error("Serialized request exceeds the Passport message limit");
    }
    return payload;
}

export function isDeviceId(value: string): boolean {
    return DEVICE_ID_PATTERN.test(value);
}
