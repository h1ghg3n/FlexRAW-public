ALTER TABLE photos
ADD COLUMN source_parent_path TEXT
CHECK (source_parent_path IS NULL OR length(trim(source_parent_path)) > 0);

CREATE INDEX idx_photos_source_parent_display_name_id
ON photos (source_parent_path, display_name COLLATE NOCASE, id);

UPDATE schema_version SET version = 7;
