CREATE TABLE object_cleanup_jobs (
    id uuid PRIMARY KEY,
    object_key text NOT NULL CHECK (
        char_length(object_key) BETWEEN 9 AND 1024
        AND object_key LIKE 'uploads/%'
    ),
    provider_upload_id text NOT NULL DEFAULT ''
        CHECK (char_length(provider_upload_id) <= 2048),
    abort_multipart boolean NOT NULL DEFAULT false,
    delete_object boolean NOT NULL DEFAULT true,
    status text NOT NULL DEFAULT 'pending'
        CHECK (status IN ('pending', 'running')),
    attempt_count integer NOT NULL DEFAULT 0 CHECK (attempt_count >= 0),
    retry_available_at timestamptz NOT NULL DEFAULT now(),
    lease_expires_at timestamptz,
    claim_token uuid,
    last_error text NOT NULL DEFAULT '' CHECK (char_length(last_error) <= 512),
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT object_cleanup_jobs_action_check CHECK (
        (abort_multipart OR delete_object)
        AND (NOT abort_multipart OR provider_upload_id <> '')
    ),
    CONSTRAINT object_cleanup_jobs_claim_check CHECK (
        (status = 'pending' AND lease_expires_at IS NULL AND claim_token IS NULL)
        OR
        (status = 'running' AND lease_expires_at IS NOT NULL AND claim_token IS NOT NULL)
    ),
    CONSTRAINT object_cleanup_jobs_identity UNIQUE (
        object_key, provider_upload_id, abort_multipart, delete_object
    )
);

CREATE INDEX object_cleanup_jobs_ready_idx
    ON object_cleanup_jobs(retry_available_at, created_at, id)
    WHERE status = 'pending';
CREATE INDEX object_cleanup_jobs_lease_idx
    ON object_cleanup_jobs(lease_expires_at, id)
    WHERE status = 'running';

-- Older completed uploads lost the staging key when object_key advanced to the
-- immutable blob key. The staging namespace is deterministic, so seed cleanup
-- jobs during the upgrade and let the normal leased worker remove them.
INSERT INTO object_cleanup_jobs (
    id, object_key, delete_object, retry_available_at, created_at, updated_at
)
SELECT gen_random_uuid(),
       'uploads/' || project_id::text || '/' || id::text,
       true, now(), now(), now()
FROM upload_sessions
WHERE status = 'completed'
ON CONFLICT (object_key, provider_upload_id, abort_multipart, delete_object)
DO NOTHING;

-- Preserve exact provider identifiers for dogfood rows already marked for
-- cleanup before this durable queue existed.
INSERT INTO object_cleanup_jobs (
    id, object_key, provider_upload_id, abort_multipart, delete_object,
    retry_available_at, created_at, updated_at
)
SELECT gen_random_uuid(), object_key,
       CASE
           WHEN upload_mode = 'multipart' AND provider_upload_id <> ''
                AND multipart_state <> 'aborted'
           THEN provider_upload_id ELSE ''
       END,
       upload_mode = 'multipart' AND provider_upload_id <> ''
           AND multipart_state <> 'aborted',
       true, now(), now(), now()
FROM upload_sessions
WHERE status IN ('aborted', 'expired')
  AND object_key LIKE 'uploads/%'
  AND char_length(object_key) BETWEEN 9 AND 1024
  AND char_length(provider_upload_id) <= 2048
ON CONFLICT (object_key, provider_upload_id, abort_multipart, delete_object)
DO NOTHING;
