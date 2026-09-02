DROP INDEX IF EXISTS project_session_members_desktop_session_active_idx;

ALTER TABLE project_session_members
    DROP CONSTRAINT IF EXISTS project_session_members_active_provenance_check,
    DROP CONSTRAINT IF EXISTS project_session_members_desktop_session_fk,
    DROP COLUMN IF EXISTS desktop_session_id;

DROP INDEX IF EXISTS desktop_sessions_identity_idx;
