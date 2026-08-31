-- Device heartbeat / connection events: each successful WebSocket hello (online)
-- and session close (offline) is recorded for the admin heartbeat-monitoring chart.
CREATE TABLE IF NOT EXISTS device_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    event TEXT NOT NULL CHECK (event IN ('online', 'offline')),
    created_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS device_events_device_created_idx
    ON device_events (device_id, created_at DESC);

CREATE INDEX IF NOT EXISTS device_events_created_idx
    ON device_events (created_at DESC);
