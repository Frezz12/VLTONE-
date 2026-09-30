CREATE TABLE project_edit_leases (
 id uuid PRIMARY KEY,
 project_id uuid NOT NULL REFERENCES cloud_projects(id) ON DELETE CASCADE,
 session_id uuid NOT NULL REFERENCES project_live_sessions(id) ON DELETE CASCADE,
 holder_member_id uuid NOT NULL REFERENCES project_session_members(id) ON DELETE CASCADE,
 field_keys jsonb NOT NULL CHECK (jsonb_typeof(field_keys) = 'array'),
 expires_at timestamptz NOT NULL
);
CREATE INDEX project_edit_leases_session_expiry ON project_edit_leases(session_id, expires_at);
