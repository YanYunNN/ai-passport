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
    const alphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    const bytes = crypto.getRandomValues(new Uint8Array(10));
    return Array.from(bytes, (byte) => alphabet[byte & 31]).join("");
}

export function normalizeUserCode(value: string): string | null {
    const normalized = value.replaceAll(/[-\s]/gu, "").toUpperCase();
    return /^[ABCDEFGHJKMNPQRSTUVWXYZ23456789]{10}$/u.test(normalized) ? normalized : null;
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
