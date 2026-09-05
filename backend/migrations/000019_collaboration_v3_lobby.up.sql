-- Protocol v3 keeps plugin compatibility and readiness at the session
-- boundary. Existing rows are v2/ready so old sessions retain their exact
-- semantics; only newly-created v3 sessions remain in `starting`.
ALTER TABLE project_live_sessions
    ADD COLUMN command_schema_version integer NOT NULL DEFAULT 2
        CHECK (command_schema_version IN (2, 3)),
    ADD COLUMN plugin_requirements_revision bigint NOT NULL DEFAULT 0
        CHECK (plugin_requirements_revision >= 0),
    ADD COLUMN plugin_requirements jsonb NOT NULL DEFAULT '[]'::jsonb
        CHECK (jsonb_typeof(plugin_requirements) = 'array');

ALTER TABLE project_session_members
    ADD COLUMN effective_role text NOT NULL DEFAULT 'viewer'
        CHECK (effective_role IN ('owner', 'editor', 'viewer')),
    ADD COLUMN readiness_status text NOT NULL DEFAULT 'ready'
        CHECK (readiness_status IN ('ready', 'blocked', 'viewer')),
    ADD COLUMN readiness_revision bigint NOT NULL DEFAULT 0
        CHECK (readiness_revision >= 0),
    ADD COLUMN plugin_readiness jsonb NOT NULL DEFAULT '[]'::jsonb
        CHECK (jsonb_typeof(plugin_readiness) = 'array');

CREATE INDEX project_session_members_readiness_idx
    ON project_session_members(session_id, readiness_status)
    WHERE left_at IS NULL;
