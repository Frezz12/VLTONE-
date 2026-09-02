ALTER TABLE project_snapshots
    DROP CONSTRAINT IF EXISTS project_snapshots_asset_ids_check,
    DROP COLUMN IF EXISTS asset_ids;

ALTER TABLE upload_sessions
    DROP CONSTRAINT IF EXISTS upload_sessions_snapshot_asset_ids_check,
    DROP COLUMN IF EXISTS snapshot_asset_ids;

DROP FUNCTION IF EXISTS collaboration_snapshot_asset_manifest_valid(jsonb);
