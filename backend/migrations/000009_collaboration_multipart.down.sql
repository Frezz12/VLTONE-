DROP INDEX IF EXISTS upload_sessions_multipart_cleanup_idx;

ALTER TABLE upload_sessions
    DROP CONSTRAINT IF EXISTS upload_sessions_multipart_manifest_check,
    DROP CONSTRAINT IF EXISTS upload_sessions_multipart_shape_check,
    DROP COLUMN IF EXISTS multipart_manifest,
    DROP COLUMN IF EXISTS multipart_state,
    DROP COLUMN IF EXISTS multipart_part_count,
    DROP COLUMN IF EXISTS multipart_part_size,
    DROP COLUMN IF EXISTS upload_mode;

