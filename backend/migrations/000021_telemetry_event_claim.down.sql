ALTER TABLE telemetry_events ALTER CONSTRAINT telemetry_events_session_id_fkey
    NOT DEFERRABLE INITIALLY IMMEDIATE;
