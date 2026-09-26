-- Separate account, contract and optional diagnostic decisions. Existing users
-- are not silently opted into the new diagnostic consent.
ALTER TABLE users
    ADD COLUMN terms_version text NOT NULL DEFAULT '',
    ADD COLUMN terms_accepted_at timestamptz,
    ADD COLUMN diagnostics_consent_version text NOT NULL DEFAULT '',
    ADD COLUMN diagnostics_accepted_at timestamptz,
    ADD COLUMN diagnostics_revoked_at timestamptz;
CREATE TABLE legal_acceptances (
    id uuid PRIMARY KEY,
    user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    purpose text NOT NULL CHECK (purpose IN ('account', 'terms', 'diagnostics')),
    version text NOT NULL,
    action text NOT NULL CHECK (action IN ('accepted', 'revoked')),
    occurred_at timestamptz NOT NULL,
    ip text NOT NULL
);
CREATE INDEX legal_acceptances_user_time_idx ON legal_acceptances(user_id, occurred_at);
