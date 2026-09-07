ALTER TABLE desktop_sessions
    ADD COLUMN refresh_request_hash text,
    ADD COLUMN rotated_to_id uuid;
