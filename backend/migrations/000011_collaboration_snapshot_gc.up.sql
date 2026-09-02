-- Snapshot creation is a durable, retryable server request. A live session is
-- not considered ended until its exact final sequence has a verified snapshot.
DO $$
DECLARE constraint_name text;
BEGIN
    FOR constraint_name IN
        SELECT conname FROM pg_constraint
        WHERE conrelid = 'project_live_sessions'::regclass
          AND contype = 'c'
          AND pg_get_constraintdef(oid) ILIKE '%status%'
    LOOP
        EXECUTE format('ALTER TABLE project_live_sessions DROP CONSTRAINT %I', constraint_name);
    END LOOP;
END $$;

ALTER TABLE project_live_sessions
    ADD CONSTRAINT project_live_sessions_status_check
        CHECK (status IN ('starting', 'active', 'ending', 'ended')),
    ADD CONSTRAINT project_live_sessions_ended_at_check
        CHECK ((status = 'ended') = (ended_at IS NOT NULL)),
    ADD CONSTRAINT project_live_sessions_host_state_check
        CHECK (status IN ('active', 'ending') OR host_member_id IS NULL);

DROP INDEX project_live_sessions_one_live_idx;
CREATE UNIQUE INDEX project_live_sessions_one_live_idx
    ON project_live_sessions(project_id)
    WHERE status IN ('starting', 'active', 'ending');

CREATE TABLE project_snapshot_requests (
    id uuid PRIMARY KEY,
    project_id uuid NOT NULL REFERENCES cloud_projects(id) ON DELETE CASCADE,
    session_id uuid NOT NULL,
    target_seq bigint NOT NULL CHECK (target_seq >= 0),
    reason text NOT NULL CHECK (reason IN ('autosave', 'session_end')),
    status text NOT NULL CHECK (status IN ('pending', 'completed', 'superseded')),
    assigned_member_id uuid,
    attempt_count integer NOT NULL DEFAULT 0 CHECK (attempt_count BETWEEN 0 AND 1000000),
    requested_at timestamptz NOT NULL DEFAULT now(),
    last_dispatched_at timestamptz,
    next_retry_at timestamptz NOT NULL DEFAULT now(),
    completed_snapshot_id uuid REFERENCES project_snapshots(id) ON DELETE SET NULL,
    completed_at timestamptz,
    FOREIGN KEY (session_id, project_id)
        REFERENCES project_live_sessions(id, project_id) ON DELETE CASCADE,
    FOREIGN KEY (assigned_member_id, session_id)
        REFERENCES project_session_members(id, session_id) ON DELETE SET NULL,
    CHECK ((status = 'completed') = (completed_at IS NOT NULL)),
    CHECK (status <> 'completed' OR completed_snapshot_id IS NOT NULL)
);
CREATE UNIQUE INDEX project_snapshot_requests_one_pending_idx
    ON project_snapshot_requests(project_id) WHERE status = 'pending';
CREATE INDEX project_snapshot_requests_due_idx
    ON project_snapshot_requests(next_retry_at, project_id) WHERE status = 'pending';
CREATE INDEX project_snapshot_requests_session_idx
    ON project_snapshot_requests(session_id, requested_at DESC);

ALTER TABLE blobs ADD COLUMN unreferenced_at timestamptz;
CREATE INDEX blobs_gc_candidates_idx
    ON blobs(unreferenced_at, id)
    WHERE status = 'ready' AND unreferenced_at IS NOT NULL;

COMMENT ON TABLE project_snapshot_requests IS
    'Server-authoritative exact-sequence requests; payload bytes remain in object storage and are never logged.';
COMMENT ON COLUMN blobs.unreferenced_at IS
    'First observed time with no project asset or retained snapshot reference; reset when a reference exists.';
