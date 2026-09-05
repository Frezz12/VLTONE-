DROP TABLE IF EXISTS project_invite_attempts;

ALTER TABLE project_live_sessions
    DROP COLUMN IF EXISTS password_set_at,
    DROP COLUMN IF EXISTS password_hash;

DROP INDEX IF EXISTS project_invites_code_lookup_key;

ALTER TABLE project_invites
    DROP COLUMN IF EXISTS locked_until,
    DROP COLUMN IF EXISTS attempt_count,
    DROP COLUMN IF EXISTS code_digits,
    DROP COLUMN IF EXISTS code_lookup;
