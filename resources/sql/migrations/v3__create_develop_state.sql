CREATE TABLE develop_params (
    photo_id INTEGER PRIMARY KEY REFERENCES photos(id) ON DELETE CASCADE,
    params_json TEXT NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

CREATE TABLE develop_history (
    id INTEGER PRIMARY KEY,
    photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,
    step_id INTEGER NOT NULL,
    params_json TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    UNIQUE(photo_id, step_id)
);

CREATE INDEX idx_develop_history_photo_step ON develop_history (photo_id, step_id);

UPDATE schema_version SET version = 3;
