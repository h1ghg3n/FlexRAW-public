CREATE TABLE IF NOT EXISTS flexraw_catalog_metadata (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    catalog_uuid TEXT NOT NULL UNIQUE CHECK (length(catalog_uuid) = 36)
);

INSERT INTO flexraw_catalog_metadata (id, catalog_uuid)
SELECT
    1,
    lower(hex(randomblob(4))) || '-' ||
    lower(hex(randomblob(2))) || '-4' ||
    substr(lower(hex(randomblob(2))), 2) || '-' ||
    substr('89ab', (random() & 3) + 1, 1) ||
    substr(lower(hex(randomblob(2))), 2) || '-' ||
    lower(hex(randomblob(6)))
WHERE NOT EXISTS (SELECT 1 FROM flexraw_catalog_metadata);

UPDATE schema_version SET version = 11;
