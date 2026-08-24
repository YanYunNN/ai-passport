import type { Env } from "./env";

export interface DeviceCredentialRecord {
    device_id: string;
    credential_hash: string;
    previous_credential_hash: string | null;
    previous_credential_expires_at: number | null;
    credential_version: number;
    status: "active" | "revoked";
}

export interface VerifiedDeviceCredential {
    record: DeviceCredentialRecord;
    credentialHash: string;
}

const encoder = new TextEncoder();

function base64Url(bytes: Uint8Array): string {
    let binary = "";
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return btoa(binary).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");
}

function constantTimeEqual(left: string, right: string): boolean {
    const leftBytes = encoder.encode(left);
    const rightBytes = encoder.encode(right);
    let difference = leftBytes.length ^ rightBytes.length;
    const length = Math.max(leftBytes.length, rightBytes.length);
    for (let index = 0; index < length; index++) {
        difference |= (leftBytes[index] ?? 0) ^ (rightBytes[index] ?? 0);
    }
    return difference === 0;
}

async function timingSafeSecretEqual(left: string, right: string): Promise<boolean> {
    const [leftHash, rightHash] = await Promise.all([
        crypto.subtle.digest("SHA-256", encoder.encode(left)),
        crypto.subtle.digest("SHA-256", encoder.encode(right)),
    ]);
    return crypto.subtle.timingSafeEqual(leftHash, rightHash);
}

function strictBasicCredentials(request: Request): { username: string; password: string } | null {
    const authorization = request.headers.get("Authorization");
    if (!authorization || /[\r\n]/u.test(authorization)) return null;
    const match = /^Basic ([A-Za-z0-9+/]+={0,2})$/iu.exec(authorization);
    if (!match || match[1].length % 4 !== 0) return null;

    try {
        const binary = atob(match[1]);
        // Only accept canonical padded base64, not the permissive variants atob may decode.
        if (btoa(binary) !== match[1]) return null;
        const decoded = new TextDecoder("utf-8", { fatal: true }).decode(
            Uint8Array.from(binary, (character) => character.charCodeAt(0)),
        );
        const separator = decoded.indexOf(":");
        if (separator <= 0) return null;
        const username = decoded.slice(0, separator);
        const password = decoded.slice(separator + 1);
        if (!password || /[\u0000-\u001f\u007f]/u.test(username) || /[\u0000-\u001f\u007f]/u.test(password)) return null;
        return { username, password };
    } catch {
        return null;
    }
}

export async function verifyAdminBasicAuth(request: Request, env: Env): Promise<string | null> {
    const supplied = strictBasicCredentials(request);
    const expectedUsername = env.ADMIN_UI_USERNAME;
    const expectedPassword = env.ADMIN_UI_PASSWORD;
    if (!supplied || !expectedUsername || !expectedPassword ||
        /[\u0000-\u001f\u007f:]/u.test(expectedUsername) || /[\u0000-\u001f\u007f]/u.test(expectedPassword)) return null;

    const [usernameMatches, passwordMatches] = await Promise.all([
        timingSafeSecretEqual(supplied.username, expectedUsername),
        timingSafeSecretEqual(supplied.password, expectedPassword),
    ]);
    return usernameMatches && passwordMatches ? expectedUsername : null;
}

export function bearerToken(request: Request): string | null {
    const value = request.headers.get("Authorization");
    if (!value?.startsWith("Bearer ")) return null;
    const token = value.slice("Bearer ".length);
    return token && !/[\r\n]/u.test(token) ? token : null;
}

export function hasBearerSecret(request: Request, expectedSecret: string): boolean {
    const supplied = bearerToken(request);
    return supplied !== null && constantTimeEqual(supplied, expectedSecret);
}

export function issueDeviceCredential(): string {
    const bytes = crypto.getRandomValues(new Uint8Array(32));
    return base64Url(bytes);
}

export function issueUserCode(): string {
    // Rejection sampling avoids the modulo bias caused by mapping 2^32 values to 1,000,000 codes.
    const limit = 0x1_0000_0000 - (0x1_0000_0000 % 1_000_000);
    const bytes = new Uint8Array(4);
    let value: number;
    do {
        crypto.getRandomValues(bytes);
        value = new DataView(bytes.buffer).getUint32(0);
    } while (value >= limit);
    return String(value % 1_000_000).padStart(6, "0");
}

export function normalizeUserCode(value: string): string | null {
    return /^[0-9]{6}$/u.test(value) ? value : null;
}

async function hashSecret(value: string, purpose: string, pepper: string): Promise<string> {
    const digest = await crypto.subtle.digest("SHA-256", encoder.encode(`${purpose}\u0000${value}\u0000${pepper}`));
    return base64Url(new Uint8Array(digest));
}

export async function hashDeviceCredential(credential: string, pepper: string): Promise<string> {
    return hashSecret(credential, "device-credential", pepper);
}

/** Device and user enrollment codes are domain-separated but both peppered before D1 storage. */
export async function hashEnrollmentCode(code: string, kind: "device-code" | "user-code", pepper: string): Promise<string> {
    return hashSecret(code, kind, pepper);
}

export async function verifyDeviceCredential(
    env: Env,
    deviceId: string,
    credential: string,
): Promise<VerifiedDeviceCredential | null> {
    const record = await env.DB.prepare(
        "SELECT device_id, credential_hash, previous_credential_hash, previous_credential_expires_at, credential_version, status " +
        "FROM devices WHERE device_id = ?1",
    ).bind(deviceId).first<DeviceCredentialRecord>();
    if (!record || record.status !== "active") return null;

    const credentialHash = await hashDeviceCredential(credential, env.DEVICE_CREDENTIAL_PEPPER);
    const currentMatches = constantTimeEqual(credentialHash, record.credential_hash);
    const previousMatches = record.previous_credential_hash !== null &&
        record.previous_credential_expires_at !== null && record.previous_credential_expires_at >= Date.now() / 1000 &&
        constantTimeEqual(credentialHash, record.previous_credential_hash);
    return currentMatches || previousMatches ? { record, credentialHash } : null;
}
