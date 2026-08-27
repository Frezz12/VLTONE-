CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE plans (
    id uuid PRIMARY KEY,
    code text NOT NULL UNIQUE,
    display_name text NOT NULL,
    monthly_token_limit bigint NOT NULL CHECK (monthly_token_limit >= 0),
    device_limit integer NOT NULL CHECK (device_limit > 0),
    all_features boolean NOT NULL DEFAULT false,
    created_at timestamptz NOT NULL DEFAULT now()
);

INSERT INTO plans (id, code, display_name, monthly_token_limit, device_limit, all_features)
VALUES ('10000000-0000-4000-8000-000000000001', 'demo', 'Demo', 20000000, 2, true);

CREATE TABLE users (
    id uuid PRIMARY KEY,
    email text NOT NULL,
    email_key text NOT NULL UNIQUE,
    nickname text NOT NULL,
    nickname_key text NOT NULL UNIQUE,
    password_hash text NOT NULL,
    locale text NOT NULL DEFAULT 'en' CHECK (locale IN ('en', 'ru')),
    status text NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'suspended')),
    consent_version text NOT NULL,
    consent_accepted_at timestamptz NOT NULL,
    consent_ip text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE admin_users (
    id uuid PRIMARY KEY,
    email text NOT NULL,
    email_key text NOT NULL UNIQUE,
    nickname text NOT NULL,
    password_hash text NOT NULL,
    status text NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'suspended')),
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE subscriptions (
    id uuid PRIMARY KEY,
    user_id uuid NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,
    plan_id uuid NOT NULL REFERENCES plans(id),
    status text NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'suspended')),
    starts_at timestamptz NOT NULL,
    ends_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE web_sessions (
    id uuid PRIMARY KEY,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash text NOT NULL UNIQUE,
    csrf_token text NOT NULL,
    last_seen_at timestamptz NOT NULL,
    expires_at timestamptz NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    revoked_at timestamptz
);
CREATE INDEX web_sessions_user_idx ON web_sessions(user_id);
CREATE INDEX web_sessions_expiry_idx ON web_sessions(expires_at);

CREATE TABLE admin_sessions (
    id uuid PRIMARY KEY,
    admin_user_id uuid NOT NULL REFERENCES admin_users(id) ON DELETE CASCADE,
    token_hash text NOT NULL UNIQUE,
    csrf_token text NOT NULL,
    last_seen_at timestamptz NOT NULL,
    expires_at timestamptz NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    revoked_at timestamptz
);
CREATE INDEX admin_sessions_user_idx ON admin_sessions(admin_user_id);

CREATE TABLE devices (
    id uuid PRIMARY KEY,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    install_id text NOT NULL,
    display_name text NOT NULL,
    platform text NOT NULL CHECK (platform IN ('windows', 'macos')),
    os_version text NOT NULL,
    app_version text NOT NULL,
    hardware jsonb NOT NULL DEFAULT '{}'::jsonb,
    first_seen_at timestamptz NOT NULL,
    last_seen_at timestamptz NOT NULL,
    revoked_at timestamptz,
    UNIQUE (user_id, install_id)
);
CREATE INDEX devices_user_active_idx ON devices(user_id, revoked_at);

CREATE TABLE desktop_sessions (
    id uuid PRIMARY KEY,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id uuid NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    refresh_token_hash text NOT NULL UNIQUE,
    reporter_token_hash text NOT NULL UNIQUE,
    reporter_expires_at timestamptz NOT NULL,
    expires_at timestamptz NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    last_seen_at timestamptz NOT NULL,
    rotated_at timestamptz,
    revoked_at timestamptz
);
CREATE INDEX desktop_sessions_user_idx ON desktop_sessions(user_id);
CREATE INDEX desktop_sessions_device_idx ON desktop_sessions(device_id);

CREATE TABLE password_reset_tokens (
    id uuid PRIMARY KEY,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash text NOT NULL UNIQUE,
    expires_at timestamptz NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    used_at timestamptz
);

CREATE TABLE token_cycles (
    id uuid PRIMARY KEY,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    starts_at timestamptz NOT NULL,
    ends_at timestamptz NOT NULL,
    base_limit bigint NOT NULL CHECK (base_limit >= 0),
    adjustment bigint NOT NULL DEFAULT 0,
    used_tokens bigint NOT NULL DEFAULT 0 CHECK (used_tokens >= 0),
    reserved_tokens bigint NOT NULL DEFAULT 0 CHECK (reserved_tokens >= 0),
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (user_id, starts_at)
);

CREATE TABLE token_reservations (
    id uuid PRIMARY KEY,
    cycle_id uuid NOT NULL REFERENCES token_cycles(id) ON DELETE CASCADE,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    provider text NOT NULL,
    model text NOT NULL,
    reserved bigint NOT NULL CHECK (reserved > 0),
    created_at timestamptz NOT NULL DEFAULT now(),
    settled_at timestamptz
);

CREATE TABLE token_ledgers (
    id uuid PRIMARY KEY,
    cycle_id uuid NOT NULL REFERENCES token_cycles(id) ON DELETE CASCADE,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    actor_admin_id uuid REFERENCES admin_users(id) ON DELETE SET NULL,
    reservation_id uuid REFERENCES token_reservations(id) ON DELETE SET NULL,
    kind text NOT NULL,
    delta bigint NOT NULL,
    balance_after bigint NOT NULL,
    metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX token_ledgers_user_created_idx ON token_ledgers(user_id, created_at DESC);

CREATE TABLE telemetry_sessions (
    id uuid PRIMARY KEY,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id uuid NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    app_version text NOT NULL,
    build_id text NOT NULL,
    started_at timestamptz NOT NULL,
    last_seen_at timestamptz NOT NULL,
    ended_at timestamptz,
    end_reason text NOT NULL DEFAULT '',
    hardware jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX telemetry_sessions_user_started_idx ON telemetry_sessions(user_id, started_at DESC);
CREATE INDEX telemetry_sessions_last_seen_idx ON telemetry_sessions(last_seen_at DESC);

CREATE TABLE telemetry_events (
    id uuid PRIMARY KEY,
    event_id uuid NOT NULL UNIQUE,
    session_id uuid NOT NULL REFERENCES telemetry_sessions(id) ON DELETE CASCADE,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id uuid NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    kind text NOT NULL,
    occurred_at timestamptz NOT NULL,
    payload jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE telemetry_samples (
    id uuid NOT NULL,
    event_id uuid NOT NULL,
    session_id uuid NOT NULL REFERENCES telemetry_sessions(id) ON DELETE CASCADE,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id uuid NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    recorded_at timestamptz NOT NULL,
    process_cpu double precision NOT NULL DEFAULT 0,
    system_cpu double precision NOT NULL DEFAULT 0,
    dsp_load double precision NOT NULL DEFAULT 0,
    dsp_peak double precision NOT NULL DEFAULT 0,
    xruns bigint NOT NULL DEFAULT 0,
    resident_bytes bigint NOT NULL DEFAULT 0,
    sample_rate double precision NOT NULL DEFAULT 0,
    buffer_frames integer NOT NULL DEFAULT 0,
    track_count integer NOT NULL DEFAULT 0,
    clip_count integer NOT NULL DEFAULT 0,
    plugin_count integer NOT NULL DEFAULT 0,
    playback_state text NOT NULL DEFAULT 'stopped',
    foreground boolean NOT NULL DEFAULT false,
    plugins jsonb NOT NULL DEFAULT '[]'::jsonb,
    PRIMARY KEY (id, recorded_at)
) PARTITION BY RANGE (recorded_at);
CREATE INDEX telemetry_samples_user_time_idx ON telemetry_samples(user_id, recorded_at DESC);
CREATE INDEX telemetry_samples_session_time_idx ON telemetry_samples(session_id, recorded_at DESC);

CREATE TABLE crash_reports (
    id uuid PRIMARY KEY,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_id uuid NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    session_id uuid REFERENCES telemetry_sessions(id) ON DELETE SET NULL,
    build_id text NOT NULL,
    app_version text NOT NULL,
    platform text NOT NULL,
    reason text NOT NULL,
    last_plugin text NOT NULL DEFAULT '',
    metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    artifact_path text NOT NULL DEFAULT '',
    artifact_bytes bigint NOT NULL DEFAULT 0,
    sha256 text NOT NULL DEFAULT '',
    occurred_at timestamptz NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX crash_reports_user_time_idx ON crash_reports(user_id, occurred_at DESC);

CREATE SEQUENCE bug_report_number_seq START 1001;
CREATE TABLE bug_reports (
    id uuid PRIMARY KEY,
    number bigint NOT NULL UNIQUE DEFAULT nextval('bug_report_number_seq'),
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title text NOT NULL,
    description text NOT NULL,
    steps text NOT NULL DEFAULT '',
    expected text NOT NULL DEFAULT '',
    actual text NOT NULL DEFAULT '',
    status text NOT NULL DEFAULT 'new' CHECK (status IN ('new', 'triage', 'in_progress', 'fixed', 'duplicate', 'wont_fix')),
    internal_note text NOT NULL DEFAULT '',
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX bug_reports_user_time_idx ON bug_reports(user_id, created_at DESC);

CREATE TABLE bug_attachments (
    id uuid PRIMARY KEY,
    bug_id uuid NOT NULL REFERENCES bug_reports(id) ON DELETE CASCADE,
    file_name text NOT NULL,
    mime_type text NOT NULL,
    path text NOT NULL,
    bytes bigint NOT NULL,
    sha256 text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE admin_audit_logs (
    id uuid PRIMARY KEY,
    admin_user_id uuid NOT NULL REFERENCES admin_users(id) ON DELETE RESTRICT,
    action text NOT NULL,
    target_type text NOT NULL,
    target_hash text NOT NULL,
    metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    ip text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX admin_audit_created_idx ON admin_audit_logs(created_at DESC);

CREATE FUNCTION prevent_admin_audit_mutation() RETURNS trigger AS $$
BEGIN
    RAISE EXCEPTION 'admin_audit_logs is append-only';
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER admin_audit_logs_immutable
BEFORE UPDATE OR DELETE ON admin_audit_logs
FOR EACH ROW EXECUTE FUNCTION prevent_admin_audit_mutation();
