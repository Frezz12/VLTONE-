DROP TRIGGER IF EXISTS project_ops_assets_ready ON project_ops;
DROP FUNCTION IF EXISTS enforce_collaboration_command_assets_ready();
DROP FUNCTION IF EXISTS collaboration_command_assets_ready(uuid, text, jsonb);

DROP INDEX IF EXISTS upload_sessions_project_snapshot_idx;
DROP INDEX IF EXISTS upload_sessions_project_asset_idx;
ALTER TABLE upload_sessions
    DROP CONSTRAINT IF EXISTS upload_sessions_purpose_check,
    DROP CONSTRAINT IF EXISTS upload_sessions_request_hash_check,
    DROP COLUMN IF EXISTS request_hash,
    DROP COLUMN IF EXISTS display_name,
    DROP COLUMN IF EXISTS content_type,
    DROP COLUMN IF EXISTS snapshot_schema_version,
    DROP COLUMN IF EXISTS snapshot_seq,
    DROP COLUMN IF EXISTS asset_id;
