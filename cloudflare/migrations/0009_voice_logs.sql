-- Voice conversation and debugging logs for online tracing.
CREATE TABLE IF NOT EXISTS voice_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    session_id TEXT,
    asr_text TEXT,
    ai_reply TEXT,
    audio_bytes INTEGER DEFAULT 0,
    mp3_bytes INTEGER DEFAULT 0,
    latency_asr_ms INTEGER DEFAULT 0,
    latency_chat_ms INTEGER DEFAULT 0,
    latency_tts_ms INTEGER DEFAULT 0,
    latency_total_ms INTEGER DEFAULT 0,
    status TEXT NOT NULL,
    error_msg TEXT,
    created_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS voice_logs_device_created_idx
    ON voice_logs (device_id, created_at DESC);

CREATE INDEX IF NOT EXISTS voice_logs_created_idx
    ON voice_logs (created_at DESC);
