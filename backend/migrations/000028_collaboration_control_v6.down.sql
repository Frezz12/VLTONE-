DROP TABLE project_bans;
ALTER TABLE cloud_projects DROP COLUMN plugin_policy;
DROP TABLE project_session_exclusions;
DROP TABLE project_session_controls;
ALTER TABLE project_live_sessions DROP CONSTRAINT project_live_sessions_host_state_check;
ALTER TABLE project_live_sessions ADD CONSTRAINT project_live_sessions_host_state_check CHECK (status IN ('active', 'ending') OR host_member_id IS NULL);
ALTER TABLE project_session_members DROP COLUMN plugin_inventory;
ALTER TABLE project_live_sessions DROP COLUMN app_version, DROP COLUMN plugin_policy,
    DROP COLUMN catalog_revision, DROP COLUMN transport_state, DROP COLUMN audition_state;
-- Refuse downgrade while v6 sessions exist; never silently reinterpret them.
ALTER TABLE project_live_sessions DROP CONSTRAINT project_live_sessions_command_schema_version_check;
ALTER TABLE project_live_sessions ADD CONSTRAINT project_live_sessions_command_schema_version_check CHECK (command_schema_version IN (2, 3, 4, 5));
