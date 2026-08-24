CREATE TABLE IF NOT EXISTS device_enrollments (
    enrollment_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    device_code_hash TEXT NOT NULL UNIQUE,
    user_code_hash TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'approved', 'denied', 'consumed', 'expired')),
    expires_at INTEGER NOT NULL,
    poll_interval_seconds INTEGER NOT NULL DEFAULT 5 CHECK (poll_interval_seconds BETWEEN 5 AND 15),
    last_polled_at INTEGER,
    approved_by TEXT,
    approved_at INTEGER,
    consumed INTEGER NOT NULL DEFAULT 0 CHECK (consumed IN (0, 1)),
    consumed_at INTEGER,
    created_at INTEGER NOT NULL
);

-- The partial index allows an expired/terminal enrollment to remain auditable while
-- enforcing at most one live enrollment awaiting approval for each device.
CREATE UNIQUE INDEX IF NOT EXISTS device_enrollments_one_pending_per_device_idx
    ON device_enrollments (device_id)
    WHERE status = 'pending';

CREATE INDEX IF NOT EXISTS device_enrollments_expiry_idx
    ON device_enrollments (expires_at);
