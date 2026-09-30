ALTER TABLE cloud_projects ADD COLUMN plugin_policy text NOT NULL DEFAULT 'external_checked' CHECK (plugin_policy IN ('builtin_only', 'external_checked'));
ALTER TABLE project_live_sessions DROP CONSTRAINT IF EXISTS project_live_sessions_command_schema_version_check;
ALTER TABLE project_live_sessions ADD CONSTRAINT project_live_sessions_command_schema_version_check CHECK (command_schema_version IN (2, 3, 4, 5, 6));
ALTER TABLE project_live_sessions DROP CONSTRAINT project_live_sessions_host_state_check;
ALTER TABLE project_live_sessions ADD CONSTRAINT project_live_sessions_host_state_check CHECK (status IN ('starting', 'active', 'ending') OR host_member_id IS NULL);
ALTER TABLE project_live_sessions
    ADD COLUMN app_version text NOT NULL DEFAULT '',
    ADD COLUMN plugin_policy text NOT NULL DEFAULT 'external_checked' CHECK (plugin_policy IN ('builtin_only', 'external_checked')),
    ADD COLUMN catalog_revision bigint NOT NULL DEFAULT 1 CHECK (catalog_revision > 0),
    ADD COLUMN transport_state jsonb NOT NULL DEFAULT '{}'::jsonb,
    ADD COLUMN audition_state jsonb NOT NULL DEFAULT '{}'::jsonb;
ALTER TABLE project_session_members ADD COLUMN plugin_inventory jsonb NOT NULL DEFAULT '[]'::jsonb;
CREATE TABLE project_session_controls (
    session_id uuid NOT NULL REFERENCES project_live_sessions(id) ON DELETE CASCADE,
    action_id uuid NOT NULL,
    actor_member_id uuid NOT NULL REFERENCES project_session_members(id),
    request_hash text NOT NULL,
    result jsonb NOT NULL,
    created_at timestamptz NOT NULL,
    PRIMARY KEY(session_id, action_id)
);
CREATE TABLE project_session_exclusions (
    session_id uuid NOT NULL REFERENCES project_live_sessions(id) ON DELETE CASCADE,
    user_id uuid NOT NULL REFERENCES users(id),
    created_by uuid NOT NULL REFERENCES users(id),
    created_at timestamptz NOT NULL,
    PRIMARY KEY(session_id, user_id)
);
CREATE TABLE project_bans (
    project_id uuid NOT NULL REFERENCES cloud_projects(id) ON DELETE CASCADE,
    user_id uuid NOT NULL REFERENCES users(id),
    created_by uuid NOT NULL REFERENCES users(id),
    created_at timestamptz NOT NULL,
    PRIMARY KEY(project_id, user_id)
);
