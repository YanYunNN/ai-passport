-- Hook notify push log: each /v1/devices/:id/notify push the Worker forwards
-- to a device (from Kiro Stop-hook or the bridge notify command) is recorded here
-- for audit and admin-dashboard querying.
CREATE TABLE IF NOT EXISTS hook_notify_log (
    id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    session_id TEXT,
    title TEXT NOT NULL,
    content TEXT NOT NULL,
    result TEXT NOT NULL CHECK (result IN ('sent', 'offline', 'error')),
    online INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS hook_notify_log_device_created_idx
    ON hook_notify_log (device_id, created_at DESC);

CREATE INDEX IF NOT EXISTS hook_notify_log_created_idx
    ON hook_notify_log (created_at DESC);