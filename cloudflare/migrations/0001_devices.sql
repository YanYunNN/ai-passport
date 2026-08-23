CREATE TABLE IF NOT EXISTS devices (
    device_id TEXT PRIMARY KEY,
    credential_hash TEXT NOT NULL,
    previous_credential_hash TEXT,
    previous_credential_expires_at INTEGER,
    credential_version INTEGER NOT NULL DEFAULT 1,
    status TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'revoked')),
    created_at INTEGER NOT NULL,
    rotated_at INTEGER
);

-- Maps a public request ID back to its device Durable Object. Terminal status is
-- a searchable projection; the Durable Object's storage remains authoritative.
CREATE TABLE IF NOT EXISTS approval_requests (
    request_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    tool TEXT NOT NULL,
    summary TEXT NOT NULL,
    expires_at INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'allow', 'deny')),
    reason TEXT,
    created_at INTEGER NOT NULL,
    decided_at INTEGER
);

CREATE INDEX IF NOT EXISTS approval_requests_device_id_idx
    ON approval_requests (device_id);

CREATE TABLE IF NOT EXISTS approval_audit (
    request_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    session_id TEXT NOT NULL,
    tool TEXT NOT NULL,
    summary TEXT NOT NULL,
    decision TEXT NOT NULL CHECK (decision IN ('allow', 'deny')),
    reason TEXT NOT NULL,
    expires_at INTEGER NOT NULL,
    decided_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS approval_audit_device_decided_at_idx
    ON approval_audit (device_id, decided_at DESC);
