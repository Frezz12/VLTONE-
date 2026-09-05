-- Short numeric invite codes, a session password, and the attempt ledger the
-- two of them need. code_lookup is a peppered HMAC, never the code: a twelve
-- digit space is small enough that an unsalted digest would be a rainbow table.
ALTER TABLE project_invites
    ADD COLUMN code_lookup text
        CHECK (code_lookup IS NULL OR char_length(code_lookup) = 64),
    ADD COLUMN code_digits smallint NOT NULL DEFAULT 0
        CHECK (code_digits BETWEEN 0 AND 32),
    ADD COLUMN attempt_count integer NOT NULL DEFAULT 0
        CHECK (attempt_count >= 0),
    ADD COLUMN locked_until timestamptz;

CREATE UNIQUE INDEX project_invites_code_lookup_key
    ON project_invites(code_lookup)
    WHERE code_lookup IS NOT NULL;

ALTER TABLE project_live_sessions
    ADD COLUMN password_hash text,
    ADD COLUMN password_set_at timestamptz,
    ADD CHECK ((password_hash IS NULL) = (password_set_at IS NULL));

-- Redemption attempts, for the per-account and per-IP sliding windows. Without
-- these, the per-invite counter does nothing against an attacker spraying
-- random codes hoping to hit any live invite. The IP is stored hashed.
CREATE TABLE project_invite_attempts (
    id uuid PRIMARY KEY,
    actor_user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    actor_ip_hash text NOT NULL CHECK (char_length(actor_ip_hash) = 64),
    invite_id uuid REFERENCES project_invites(id) ON DELETE SET NULL,
    succeeded boolean NOT NULL DEFAULT false,
    attempted_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX project_invite_attempts_user_idx
    ON project_invite_attempts(actor_user_id, attempted_at DESC);
CREATE INDEX project_invite_attempts_ip_idx
    ON project_invite_attempts(actor_ip_hash, attempted_at DESC);
CREATE INDEX project_invite_attempts_gc_idx
    ON project_invite_attempts(attempted_at);
