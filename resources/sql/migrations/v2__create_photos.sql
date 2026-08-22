CREATE TABLE photos (
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    extension TEXT NOT NULL,
    display_name TEXT NOT NULL,
    kind INTEGER NOT NULL CHECK (kind IN (1, 2)),
    scan_status INTEGER NOT NULL CHECK (scan_status BETWEEN 0 AND 3),
    file_mtime_ms INTEGER NOT NULL,
    imported_at_ms INTEGER NOT NULL
);

CREATE INDEX idx_photos_display_name ON photos (display_name COLLATE NOCASE);

UPDATE schema_version SET version = 2;
