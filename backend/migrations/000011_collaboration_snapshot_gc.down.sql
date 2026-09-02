DROP INDEX IF EXISTS blobs_gc_candidates_idx;
ALTER TABLE blobs DROP COLUMN IF EXISTS unreferenced_at;

DROP TABLE IF EXISTS project_snapshot_requests;

DROP INDEX IF EXISTS project_live_sessions_one_live_idx;
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
-- The pre-000011 schema has no recoverable `ending` state. Reopen these rows
-- instead of falsely marking an unsnapshotted head as ended during rollback.
UPDATE project_live_sessions SET status = 'active', updated_at = now()
WHERE status = 'ending';
ALTER TABLE project_live_sessions
    ADD CONSTRAINT project_live_sessions_status_check
        CHECK (status IN ('starting', 'active', 'ended')),
    ADD CONSTRAINT project_live_sessions_ended_at_check
        CHECK ((status = 'ended') = (ended_at IS NOT NULL)),
    ADD CONSTRAINT project_live_sessions_host_state_check
        CHECK (status = 'active' OR host_member_id IS NULL);
CREATE UNIQUE INDEX project_live_sessions_one_live_idx
    ON project_live_sessions(project_id) WHERE status IN ('starting', 'active');
