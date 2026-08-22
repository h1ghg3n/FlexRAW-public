CREATE TABLE photos_v5 (
    id INTEGER PRIMARY KEY,
    source_path TEXT UNIQUE,
    last_known_path TEXT NOT NULL CHECK (length(trim(last_known_path)) > 0),
    extension TEXT NOT NULL,
    display_name TEXT NOT NULL,
    kind INTEGER NOT NULL CHECK (kind IN (1, 2)),
    scan_status INTEGER NOT NULL CHECK (scan_status BETWEEN 0 AND 3),
    source_size_bytes INTEGER NOT NULL CHECK (source_size_bytes >= 0),
    source_mtime_ms INTEGER NOT NULL CHECK (source_mtime_ms >= 0),
    source_sha256 BLOB CHECK (source_sha256 IS NULL OR length(source_sha256) = 32),
    source_binding_state INTEGER NOT NULL CHECK (source_binding_state BETWEEN 0 AND 7),
    imported_at_ms INTEGER NOT NULL
);

INSERT INTO photos_v5 (
    id,
    source_path,
    last_known_path,
    extension,
    display_name,
    kind,
    scan_status,
    source_size_bytes,
    source_mtime_ms,
    source_sha256,
    source_binding_state,
    imported_at_ms
)
SELECT
    id,
    path,
    path,
    extension,
    display_name,
    kind,
    scan_status,
    0,
    MAX(file_mtime_ms, 0),
    NULL,
    0,
    imported_at_ms
FROM photos;

CREATE TABLE develop_params_v5 (
    photo_id INTEGER PRIMARY KEY REFERENCES photos_v5(id) ON DELETE CASCADE,
    params_json TEXT NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

INSERT INTO develop_params_v5 (photo_id, params_json, updated_at_ms)
SELECT photo_id, params_json, updated_at_ms FROM develop_params;

CREATE TABLE develop_history_v5 (
    id INTEGER PRIMARY KEY,
    photo_id INTEGER NOT NULL REFERENCES photos_v5(id) ON DELETE CASCADE,
    step_id INTEGER NOT NULL,
    params_json TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    UNIQUE(photo_id, step_id)
);

INSERT INTO develop_history_v5 (id, photo_id, step_id, params_json, created_at_ms)
SELECT id, photo_id, step_id, params_json, created_at_ms FROM develop_history;

DROP TABLE develop_history;
DROP TABLE develop_params;
DROP TABLE photos;

ALTER TABLE photos_v5 RENAME TO photos;
ALTER TABLE develop_params_v5 RENAME TO develop_params;
ALTER TABLE develop_history_v5 RENAME TO develop_history;

CREATE INDEX idx_photos_display_name ON photos (display_name COLLATE NOCASE);
CREATE INDEX idx_develop_history_photo_step ON develop_history (photo_id, step_id);

UPDATE schema_version SET version = 5;
