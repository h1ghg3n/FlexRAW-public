CREATE TABLE presets (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL CHECK (length(trim(name)) > 0),
    category TEXT NOT NULL DEFAULT '',
    params_json TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    UNIQUE(category, name)
);

CREATE INDEX idx_presets_category_name ON presets (category COLLATE NOCASE, name COLLATE NOCASE);

UPDATE schema_version SET version = 4;
