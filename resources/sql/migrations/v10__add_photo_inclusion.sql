ALTER TABLE photos
ADD COLUMN inclusion_state INTEGER NOT NULL DEFAULT 0
CHECK (inclusion_state IN (0, 1));

CREATE INDEX idx_photos_inclusion_display_name_id
ON photos (inclusion_state, display_name COLLATE NOCASE, id);

CREATE INDEX idx_photos_parent_inclusion_display_name_id
ON photos (source_parent_path, inclusion_state, display_name COLLATE NOCASE, id);

UPDATE schema_version SET version = 10;
