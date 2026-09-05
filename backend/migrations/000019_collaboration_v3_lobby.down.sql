DROP INDEX IF EXISTS project_session_members_readiness_idx;

ALTER TABLE project_session_members
    DROP COLUMN IF EXISTS plugin_readiness,
    DROP COLUMN IF EXISTS readiness_revision,
    DROP COLUMN IF EXISTS readiness_status,
    DROP COLUMN IF EXISTS effective_role;

ALTER TABLE project_live_sessions
    DROP COLUMN IF EXISTS plugin_requirements,
    DROP COLUMN IF EXISTS plugin_requirements_revision,
    DROP COLUMN IF EXISTS command_schema_version;
