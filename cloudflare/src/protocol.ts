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

export interface DeviceNotify {
    v: 1;
    type: "notify";
    device_id: string;
    session_id: string;
    id: string;
    title: string;
    content: string;
    ts: number;
}

// The device's control buffer is 1024 bytes; keep the full serialized "notify"
// within a safe margin at 1000 bytes (unlike MAX_MESSAGE_BYTES which is the
// device->worker request limit). Inputs are pre-sanitized printable ASCII so no
// JSON escaping is required, matching the firmware's unescaped parser grammar.
const MAX_NOTIFY_BYTES = 1000;

/**
 * Build the exact "notify" payload. Truncates `content` (and validates every
 * other field) so the serialized message always fits and stays printable-ASCII
 * and NUL-safe. Throws if a fixed/generated field makes the message impossible.
 */
export function serializeDeviceNotify(notify: Omit<DeviceNotify, "type" | "v">): string {
    // Same narrow sheet as `printableAscii`: space..~ excluding `"` (0x22) and
    // `\` (0x5c), so the unescaped payload can never break the firmware parser.
    const sanitize = (value: string, maxLength: number, fallback: string): string => {
        const cleaned = [...value.replace(/[^ -!#-\[\]-~]/gu, "")];
        return (cleaned.length > 0 ? cleaned.join("") : fallback).slice(0, maxLength);
    };

    const deviceId = sanitize(notify.device_id, 31, "");
    const sessionId = sanitize(notify.session_id, 64, "");
    const id = sanitize(notify.id, 64, "");
    const title = sanitize(notify.title, 32, "Agent");
    if (!deviceId || !sessionId || !id || !Number.isSafeInteger(notify.ts)) {
        throw new Error("Invalid notify payload: missing or non-sanitizable fields");
    }

    // Reserve room for the fixed prefix/suffix and a trailing "..." crop marker.
    const prefix = `{"v":1,"type":"notify","device_id":"${deviceId}",` +
        `"session_id":"${sessionId}","id":"${id}","title":"${title}","content":"`;
    const suffix = `","ts":${notify.ts}}`;
    const prefixBytes = new TextEncoder().encode(prefix).byteLength;
    const suffixBytes = new TextEncoder().encode(suffix).byteLength;
    const reserved = prefixBytes + suffixBytes;
    if (reserved > MAX_NOTIFY_BYTES) throw new Error("Notify header exceeds the device buffer limit");

    const room = MAX_NOTIFY_BYTES - reserved;
    let content = [...notify.content].filter((character) => /[ -!#-\[\]-~]/.test(character)).join("");
    const encode = (value: string): number => new TextEncoder().encode(value).byteLength;
    if (encode(content) > room) {
        // Keep the message printable-ASCII and NUL-safe while fitting: trim the
        // content (one code unit at a time) and append an ASCII "..." crop marker.
        const ellipsis = "...".slice(0, Math.max(0, Math.min(room - 1, 3)));
        const ellipsisBytes = encode(ellipsis);
        const available = room - ellipsisBytes;
        let last = content.length;
        while (last > 0 && encode(content.slice(0, last)) > available) last--;
        content = content.slice(0, last) + ellipsis;
    }

    const payload = `${prefix}${content}${suffix}`;
    const bytes = new TextEncoder().encode(payload);
    if (bytes.byteLength <= 0 || bytes.byteLength > MAX_NOTIFY_BYTES || /[^\x20-\x7e]/.test(payload) || payload.includes("\u0000")) {
        throw new Error("Serialized notify exceeds device buffer limit or is not printable ASCII");
    }
    return payload;
}

export function isDeviceId(value: string): boolean {
    return DEVICE_ID_PATTERN.test(value);
}

export interface ScreencastMessage {
    v: 1;
    type: "screencast";
    seq: number;
    slice: number;
    total: number;
    y: number;
    lines: number;
    w?: number;
    data: string;
}

export function parseScreencastMessage(payload: string): ScreencastMessage | null {
    try {
        const obj = JSON.parse(payload) as Partial<ScreencastMessage>;
        if (obj.v === 1 && obj.type === "screencast" &&
            Number.isSafeInteger(obj.seq) && (obj.seq as number) >= 0 &&
            Number.isSafeInteger(obj.slice) && (obj.slice as number) >= 0 &&
            Number.isSafeInteger(obj.total) && (obj.total as number) > 0 &&
            (obj.slice as number) < (obj.total as number) &&
            Number.isSafeInteger(obj.y) && (obj.y as number) >= 0 &&
            Number.isSafeInteger(obj.lines) && (obj.lines as number) > 0 &&
            typeof obj.data === "string" && obj.data.length > 0 && obj.data.length <= 4096) {
            return obj as ScreencastMessage;
        }
    } catch {}
    return null;
}

const SCREENCAST_PACKET_HEADER_BYTES = 16;
const SCREENCAST_PACKET_WIDTH = 240;
const SCREENCAST_PACKET_LINES = 4;
const SCREENCAST_PACKET_BYTES = SCREENCAST_PACKET_HEADER_BYTES +
    SCREENCAST_PACKET_WIDTH * SCREENCAST_PACKET_LINES * 2;

export function parseBinaryScreencastMessage(payload: ArrayBuffer): ScreencastMessage | null {
    if (payload.byteLength !== SCREENCAST_PACKET_BYTES) return null;

    const view = new DataView(payload);
    const lines = view.getUint8(3);
    const seq = view.getUint32(4);
    const slice = view.getUint16(8);
    const total = view.getUint16(10);
    const y = view.getUint16(12);
    const width = view.getUint16(14);
    if (view.getUint8(0) !== 0x53 || view.getUint8(1) !== 0x43 || view.getUint8(2) !== 1 ||
        lines !== SCREENCAST_PACKET_LINES || width !== SCREENCAST_PACKET_WIDTH ||
        total !== 80 || slice >= total || y !== slice * lines) return null;

    const pixels = new Uint8Array(payload, SCREENCAST_PACKET_HEADER_BYTES);
    let binary = "";
    for (let offset = 0; offset < pixels.length; offset += 0x8000) {
        binary += String.fromCharCode(...pixels.subarray(offset, offset + 0x8000));
    }
    return {
        v: 1,
        type: "screencast",
        seq,
        slice,
        total,
        y,
        lines,
        w: width,
        data: btoa(binary),
    };
}
