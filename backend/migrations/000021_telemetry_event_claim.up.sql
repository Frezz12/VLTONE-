-- Register the event before applying session-start side effects in the same
-- transaction. Referential integrity is still checked before commit.
ALTER TABLE telemetry_events ALTER CONSTRAINT telemetry_events_session_id_fkey
    DEFERRABLE INITIALLY DEFERRED;
