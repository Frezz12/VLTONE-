-- The maintenance reaper scans only live memberships ordered by heartbeat
-- age. Keeping the predicate in the index avoids a growing scan over session
-- history after rooms have ended.
CREATE INDEX project_session_members_stale_active_idx
    ON project_session_members(last_seen_at, session_id, id)
    WHERE left_at IS NULL;
