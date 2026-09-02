ALTER TABLE upload_sessions DROP CONSTRAINT upload_sessions_status_check;
ALTER TABLE upload_sessions
    ADD COLUMN verification_started_at timestamptz,
    ADD CONSTRAINT upload_sessions_status_check
        CHECK (status IN ('pending', 'uploading', 'verifying', 'completed', 'aborted', 'expired')),
    ADD CONSTRAINT upload_sessions_verification_state_check
        CHECK ((status = 'verifying') = (verification_started_at IS NOT NULL));
CREATE INDEX upload_sessions_verifying_idx
    ON upload_sessions(verification_started_at, id)
    WHERE status = 'verifying';
