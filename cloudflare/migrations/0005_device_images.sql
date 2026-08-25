CREATE TABLE IF NOT EXISTS device_images (
    image_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    title TEXT,
    image_data TEXT NOT NULL,
    created_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS device_images_device_idx
    ON device_images (device_id, created_at DESC);
