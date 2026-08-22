ALTER TABLE develop_params
ADD COLUMN revision INTEGER NOT NULL DEFAULT 1 CHECK (revision > 0);

UPDATE schema_version SET version = 6;
