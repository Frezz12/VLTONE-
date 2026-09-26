-- Refuse rollback while V4 sessions still exist; do not relabel their protocol.
ALTER TABLE project_live_sessions DROP CONSTRAINT IF EXISTS project_live_sessions_command_schema_version_check;
ALTER TABLE project_live_sessions ADD CONSTRAINT project_live_sessions_command_schema_version_check CHECK (command_schema_version IN (2, 3));
