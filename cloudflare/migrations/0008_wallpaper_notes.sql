-- Admin-editable wallpaper notes: short schedule / memo lines rendered on the
-- generated info wallpaper. One row per key; the admin page reads+writes them.
CREATE TABLE IF NOT EXISTS wallpaper_notes (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);
