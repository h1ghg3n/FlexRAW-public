ALTER TABLE photos
ADD COLUMN culling_state INTEGER NOT NULL DEFAULT 0
CHECK (culling_state IN (0, 1, 2));

CREATE INDEX idx_photos_culling_display_name_id
ON photos (culling_state, display_name COLLATE NOCASE, id);

CREATE INDEX idx_photos_parent_culling_display_name_id
ON photos (source_parent_path, culling_state, display_name COLLATE NOCASE, id);

UPDATE schema_version SET version = 9;
