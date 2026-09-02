DROP TABLE IF EXISTS project_snapshot_assets;
DROP TABLE IF EXISTS project_operation_assets;
ALTER TABLE project_snapshots
    DROP CONSTRAINT IF EXISTS project_snapshots_id_project_unique;
