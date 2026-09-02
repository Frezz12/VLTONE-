ALTER TABLE project_session_members
    ADD COLUMN desktop_session_id uuid;

-- Existing active memberships predate desktop-session provenance. Backfill
-- only when the same user/device still has a valid desktop session. Rows that
-- cannot be attributed remain nullable legacy history and are never accepted
-- by the exact-session store checks added with this migration.
UPDATE project_session_members AS members
SET desktop_session_id = (
    SELECT sessions.id
    FROM desktop_sessions AS sessions
    WHERE sessions.user_id = members.user_id
      AND sessions.device_id = members.device_id
      AND sessions.revoked_at IS NULL
      AND sessions.expires_at > now()
    ORDER BY sessions.created_at DESC, sessions.id DESC
    LIMIT 1
)
WHERE members.left_at IS NULL;

CREATE UNIQUE INDEX desktop_sessions_identity_idx
    ON desktop_sessions(id, user_id, device_id);

ALTER TABLE project_session_members
    ADD CONSTRAINT project_session_members_desktop_session_fk
        FOREIGN KEY (desktop_session_id, user_id, device_id)
        REFERENCES desktop_sessions(id, user_id, device_id)
        ON DELETE CASCADE,
    ADD CONSTRAINT project_session_members_active_provenance_check
        CHECK (left_at IS NOT NULL OR desktop_session_id IS NOT NULL) NOT VALID;

CREATE INDEX project_session_members_desktop_session_active_idx
    ON project_session_members(desktop_session_id)
    WHERE left_at IS NULL;
