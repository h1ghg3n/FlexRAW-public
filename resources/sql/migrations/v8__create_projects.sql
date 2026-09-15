CREATE TABLE projects (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL CHECK (length(trim(name)) > 0)
);

CREATE INDEX idx_projects_name_id
ON projects (name COLLATE NOCASE, id);

CREATE TABLE project_photos (
    project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,
    PRIMARY KEY (project_id, photo_id)
);

CREATE INDEX idx_project_photos_photo_project
ON project_photos (photo_id, project_id);

UPDATE schema_version SET version = 8;
