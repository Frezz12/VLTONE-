ALTER TABLE upload_sessions
    ADD COLUMN upload_mode text NOT NULL DEFAULT 'single',
    ADD COLUMN multipart_part_size bigint,
    ADD COLUMN multipart_part_count integer,
    ADD COLUMN multipart_state text NOT NULL DEFAULT 'none',
    ADD COLUMN multipart_manifest jsonb NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE upload_sessions
    ADD CONSTRAINT upload_sessions_multipart_shape_check CHECK (
        (
            upload_mode = 'single'
            AND multipart_part_size IS NULL
            AND multipart_part_count IS NULL
            AND multipart_state = 'none'
            AND multipart_manifest = '[]'::jsonb
        )
        OR
        (
            upload_mode = 'multipart'
            AND multipart_part_size BETWEEN 5242880 AND 5368709120
            AND multipart_part_count BETWEEN 2 AND 1000
            AND multipart_state IN (
                'creating', 'open', 'completing', 'assembled', 'aborting', 'aborted'
            )
            AND (
                (multipart_state = 'creating' AND provider_upload_id = '')
                OR
                (multipart_state <> 'creating'
                    AND char_length(provider_upload_id) BETWEEN 1 AND 2048)
            )
            AND CASE
                WHEN jsonb_typeof(multipart_manifest) = 'array'
                    THEN jsonb_array_length(multipart_manifest) <= 1000
                ELSE false
            END
        )
    ),
    ADD CONSTRAINT upload_sessions_multipart_manifest_check
        CHECK (jsonb_typeof(multipart_manifest) = 'array');

CREATE INDEX upload_sessions_multipart_cleanup_idx
    ON upload_sessions(status, expires_at)
    WHERE upload_mode = 'multipart';
