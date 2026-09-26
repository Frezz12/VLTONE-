DROP TABLE legal_acceptances;
ALTER TABLE users
    DROP COLUMN diagnostics_revoked_at,
    DROP COLUMN diagnostics_accepted_at,
    DROP COLUMN diagnostics_consent_version,
    DROP COLUMN terms_accepted_at,
    DROP COLUMN terms_version;
