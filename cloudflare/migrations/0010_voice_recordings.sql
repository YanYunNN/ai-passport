-- Audio persistence for voice logs (user recording WAV + AI reply MP3)
CREATE TABLE IF NOT EXISTS voice_recordings (
    id INTEGER PRIMARY KEY,
    audio_wav BLOB,
    tts_mp3 BLOB,
    created_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS voice_recordings_created_idx
    ON voice_recordings (created_at DESC);
