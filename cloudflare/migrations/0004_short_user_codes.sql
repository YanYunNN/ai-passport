-- Keep previous migrations immutable. Six-digit user codes are reusable after an
-- enrollment reaches a terminal state, so their hash must not be globally unique.

CREATE TABLE device_enrollments_rebuilt (
    enrollment_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    device_code_hash TEXT NOT NULL UNIQUE,
    user_code_hash TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'approved', 'denied', 'consumed', 'expired')),
    expires_at INTEGER NOT NULL,
    poll_interval_seconds INTEGER NOT NULL DEFAULT 5 CHECK (poll_interval_seconds BETWEEN 5 AND 15),
    last_polled_at INTEGER,
    approved_by TEXT,
    approved_at INTEGER,
    consumed INTEGER NOT NULL DEFAULT 0 CHECK (consumed IN (0, 1)),
    consumed_at INTEGER,
    created_at INTEGER NOT NULL,
    approved_subject TEXT
);

INSERT INTO device_enrollments_rebuilt (
    enrollment_id, device_id, device_code_hash, user_code_hash, status, expires_at,
    poll_interval_seconds, last_polled_at, approved_by, approved_at, consumed,
    consumed_at, created_at, approved_subject
)
SELECT
    enrollment_id, device_id, device_code_hash, user_code_hash, status, expires_at,
    poll_interval_seconds, last_polled_at, approved_by, approved_at, consumed,
    consumed_at, created_at, approved_subject
FROM device_enrollments;

DROP TABLE device_enrollments;
ALTER TABLE device_enrollments_rebuilt RENAME TO device_enrollments;

CREATE UNIQUE INDEX device_enrollments_one_pending_per_device_idx
    ON device_enrollments (device_id)
    WHERE status = 'pending';
CREATE UNIQUE INDEX device_enrollments_one_live_user_code_idx
    ON device_enrollments (user_code_hash)
    WHERE status IN ('pending', 'approved');
CREATE INDEX device_enrollments_expiry_idx ON device_enrollments (expires_at);
