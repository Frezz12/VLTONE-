DROP INDEX IF EXISTS upload_sessions_verifying_idx;
ALTER TABLE upload_sessions DROP CONSTRAINT upload_sessions_verification_state_check;
UPDATE upload_sessions SET status = 'uploading', verification_started_at = NULL
WHERE status = 'verifying';
ALTER TABLE upload_sessions DROP CONSTRAINT upload_sessions_status_check;
ALTER TABLE upload_sessions
    ADD CONSTRAINT upload_sessions_status_check
        CHECK (status IN ('pending', 'uploading', 'completed', 'aborted', 'expired'));
ALTER TABLE upload_sessions DROP COLUMN verification_started_at;
