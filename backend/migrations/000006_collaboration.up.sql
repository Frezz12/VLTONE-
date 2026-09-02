CREATE TABLE cloud_projects (
    id uuid PRIMARY KEY,
    owner_user_id uuid NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    title text NOT NULL CHECK (char_length(btrim(title)) BETWEEN 1 AND 160),
    status text NOT NULL DEFAULT 'uploading' CHECK (status IN ('uploading', 'active', 'read_only', 'conflict', 'archived')),
    format_version integer NOT NULL CHECK (format_version > 0),
    engine_version text NOT NULL DEFAULT '' CHECK (char_length(engine_version) <= 64),
    minimum_app_version text NOT NULL DEFAULT '' CHECK (char_length(minimum_app_version) <= 64),
    head_seq bigint NOT NULL DEFAULT 0 CHECK (head_seq >= 0),
    snapshot_seq bigint NOT NULL DEFAULT 0 CHECK (snapshot_seq >= 0 AND snapshot_seq <= head_seq),
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    archived_at timestamptz,
    CHECK ((status = 'archived') = (archived_at IS NOT NULL))
);
CREATE INDEX cloud_projects_owner_updated_idx ON cloud_projects(owner_user_id, updated_at DESC);
CREATE INDEX cloud_projects_active_updated_idx ON cloud_projects(updated_at DESC) WHERE status = 'active';

CREATE TABLE project_members (
    project_id uuid NOT NULL REFERENCES cloud_projects(id) ON DELETE CASCADE,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role text NOT NULL CHECK (role IN ('editor', 'viewer')),
    color_index smallint NOT NULL DEFAULT 0 CHECK (color_index BETWEEN 0 AND 31),
    invited_by uuid REFERENCES users(id) ON DELETE SET NULL,
    joined_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (project_id, user_id)
);
CREATE INDEX project_members_user_joined_idx ON project_members(user_id, joined_at DESC);

CREATE TABLE project_invites (
    id uuid PRIMARY KEY,
    project_id uuid NOT NULL REFERENCES cloud_projects(id) ON DELETE CASCADE,
    invited_by uuid REFERENCES users(id) ON DELETE SET NULL,
    target_email_key text,
    role text NOT NULL CHECK (role IN ('editor', 'viewer')),
    token_hash text NOT NULL UNIQUE CHECK (char_length(token_hash) = 64),
    expires_at timestamptz NOT NULL,
    accepted_by uuid REFERENCES users(id) ON DELETE SET NULL,
    accepted_at timestamptz,
    revoked_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    CHECK ((accepted_by IS NULL) = (accepted_at IS NULL)),
    CHECK (accepted_at IS NULL OR revoked_at IS NULL)
);
CREATE INDEX project_invites_project_pending_idx
    ON project_invites(project_id, expires_at)
    WHERE accepted_at IS NULL AND revoked_at IS NULL;
CREATE INDEX project_invites_target_pending_idx
    ON project_invites(target_email_key, expires_at)
    WHERE target_email_key IS NOT NULL AND accepted_at IS NULL AND revoked_at IS NULL;

CREATE TABLE project_ops (
    project_id uuid NOT NULL REFERENCES cloud_projects(id) ON DELETE CASCADE,
    seq bigint NOT NULL CHECK (seq > 0),
    op_id uuid NOT NULL,
    transaction_id uuid,
    actor_user_id uuid REFERENCES users(id) ON DELETE SET NULL,
    actor_device_id uuid REFERENCES devices(id) ON DELETE SET NULL,
    kind text NOT NULL CHECK (char_length(kind) BETWEEN 1 AND 100),
    schema_version integer NOT NULL DEFAULT 1 CHECK (schema_version > 0),
    base_seq bigint NOT NULL CHECK (base_seq >= 0),
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload) = 'object'),
    preconditions jsonb NOT NULL DEFAULT '[]'::jsonb CHECK (jsonb_typeof(preconditions) = 'array'),
    touched_fields jsonb NOT NULL DEFAULT '[]'::jsonb CHECK (jsonb_typeof(touched_fields) = 'array'),
    request_hash text NOT NULL CHECK (request_hash ~ '^[0-9a-f]{64}$'),
    created_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (project_id, seq),
    UNIQUE (project_id, op_id),
    UNIQUE (project_id, seq, op_id)
);
CREATE INDEX project_ops_actor_time_idx ON project_ops(actor_user_id, created_at DESC);
COMMENT ON COLUMN project_ops.touched_fields IS
    'Canonical field-head keys re-derived and verified by the server from the v1 command kind and payload.';

CREATE TABLE project_field_heads (
    project_id uuid NOT NULL,
    field_key text NOT NULL CHECK (char_length(field_key) BETWEEN 1 AND 512),
    head_seq bigint NOT NULL CHECK (head_seq > 0),
    head_op_id uuid NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (project_id, field_key),
    FOREIGN KEY (project_id, head_seq, head_op_id)
        REFERENCES project_ops(project_id, seq, op_id) ON DELETE CASCADE
);

CREATE TABLE blobs (
    id uuid PRIMARY KEY,
    sha256 text NOT NULL UNIQUE CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    bytes bigint NOT NULL CHECK (bytes >= 0),
    content_type text NOT NULL CHECK (char_length(content_type) BETWEEN 1 AND 160),
    kind text NOT NULL CHECK (kind IN ('audio', 'sample', 'plugin_state', 'project_snapshot', 'other')),
    object_key text NOT NULL UNIQUE CHECK (char_length(object_key) BETWEEN 1 AND 1024),
    status text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'ready', 'quarantined', 'deleting')),
    created_by uuid REFERENCES users(id) ON DELETE SET NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    verified_at timestamptz
);
CREATE INDEX blobs_status_created_idx ON blobs(status, created_at);

CREATE TABLE project_assets (
    project_id uuid NOT NULL REFERENCES cloud_projects(id) ON DELETE CASCADE,
    asset_id uuid NOT NULL,
    blob_id uuid NOT NULL REFERENCES blobs(id) ON DELETE RESTRICT,
    kind text NOT NULL CHECK (kind IN ('audio', 'sample', 'plugin_state', 'other')),
    display_name text NOT NULL DEFAULT '' CHECK (char_length(display_name) <= 255),
    created_by uuid REFERENCES users(id) ON DELETE SET NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (project_id, asset_id)
);
CREATE INDEX project_assets_blob_idx ON project_assets(blob_id);

CREATE TABLE upload_sessions (
    id uuid PRIMARY KEY,
    project_id uuid NOT NULL REFERENCES cloud_projects(id) ON DELETE CASCADE,
    blob_id uuid REFERENCES blobs(id) ON DELETE SET NULL,
    created_by uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id uuid REFERENCES devices(id) ON DELETE SET NULL,
    expected_sha256 text NOT NULL CHECK (expected_sha256 ~ '^[0-9a-f]{64}$'),
    expected_bytes bigint NOT NULL CHECK (expected_bytes >= 0),
    kind text NOT NULL CHECK (kind IN ('audio', 'sample', 'plugin_state', 'project_snapshot', 'other')),
    provider_upload_id text NOT NULL DEFAULT '',
    object_key text NOT NULL CHECK (char_length(object_key) BETWEEN 1 AND 1024),
    status text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'uploading', 'completed', 'aborted', 'expired')),
    expires_at timestamptz NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    completed_at timestamptz
);
CREATE INDEX upload_sessions_expiry_idx ON upload_sessions(status, expires_at);
CREATE INDEX upload_sessions_project_created_idx ON upload_sessions(project_id, created_at DESC);

CREATE TABLE project_snapshots (
    id uuid PRIMARY KEY,
    project_id uuid NOT NULL REFERENCES cloud_projects(id) ON DELETE CASCADE,
    seq bigint NOT NULL CHECK (seq >= 0),
    blob_id uuid NOT NULL REFERENCES blobs(id) ON DELETE RESTRICT,
    schema_version integer NOT NULL CHECK (schema_version > 0),
    created_by uuid REFERENCES users(id) ON DELETE SET NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (project_id, seq)
);
CREATE INDEX project_snapshots_project_latest_idx ON project_snapshots(project_id, seq DESC);

CREATE TABLE project_live_sessions (
    id uuid PRIMARY KEY,
    project_id uuid NOT NULL REFERENCES cloud_projects(id) ON DELETE CASCADE,
    created_by uuid REFERENCES users(id) ON DELETE SET NULL,
    host_member_id uuid,
    mode text NOT NULL DEFAULT 'independent' CHECK (mode IN ('independent', 'follow_host', 'synchronized')),
    status text NOT NULL DEFAULT 'starting' CHECK (status IN ('starting', 'active', 'ended')),
    version bigint NOT NULL DEFAULT 1 CHECK (version > 0),
    created_at timestamptz NOT NULL DEFAULT now(),
    started_at timestamptz,
    updated_at timestamptz NOT NULL DEFAULT now(),
    ended_at timestamptz,
    UNIQUE (id, project_id),
    CHECK ((status = 'ended') = (ended_at IS NOT NULL)),
    CHECK (status = 'active' OR host_member_id IS NULL)
);
CREATE UNIQUE INDEX project_live_sessions_one_live_idx
    ON project_live_sessions(project_id) WHERE status IN ('starting', 'active');
CREATE INDEX project_live_sessions_project_time_idx ON project_live_sessions(project_id, created_at DESC);

CREATE TABLE project_session_members (
    id uuid PRIMARY KEY,
    session_id uuid NOT NULL REFERENCES project_live_sessions(id) ON DELETE CASCADE,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id uuid NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    joined_at timestamptz NOT NULL DEFAULT now(),
    last_seen_at timestamptz NOT NULL DEFAULT now(),
    left_at timestamptz,
    UNIQUE (id, session_id)
);
CREATE UNIQUE INDEX project_session_members_one_active_idx
    ON project_session_members(session_id, user_id, device_id) WHERE left_at IS NULL;
CREATE INDEX project_session_members_session_active_idx
    ON project_session_members(session_id, joined_at, id) WHERE left_at IS NULL;

ALTER TABLE project_live_sessions
    ADD CONSTRAINT project_live_sessions_host_member_fk
    FOREIGN KEY (host_member_id) REFERENCES project_session_members(id) ON DELETE SET NULL;

CREATE TABLE project_leases (
    id uuid PRIMARY KEY,
    project_id uuid NOT NULL,
    session_id uuid NOT NULL,
    track_id uuid NOT NULL,
    lease_kind text NOT NULL DEFAULT 'record' CHECK (lease_kind IN ('record')),
    holder_member_id uuid NOT NULL,
    acquired_at timestamptz NOT NULL DEFAULT now(),
    renewed_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL,
    CHECK (expires_at > renewed_at),
    UNIQUE (project_id, track_id, lease_kind),
    FOREIGN KEY (session_id, project_id) REFERENCES project_live_sessions(id, project_id) ON DELETE CASCADE,
    FOREIGN KEY (holder_member_id, session_id) REFERENCES project_session_members(id, session_id) ON DELETE CASCADE
);
CREATE INDEX project_leases_expiry_idx ON project_leases(expires_at);
CREATE INDEX project_leases_holder_idx ON project_leases(holder_member_id);
