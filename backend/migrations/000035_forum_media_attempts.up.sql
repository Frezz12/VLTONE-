-- Retain output prefixes before processing starts, so a killed worker cannot
-- orphan partial video variants (even if the attachment itself was deleted).
CREATE TABLE forum_media_attempts (prefix text PRIMARY KEY, expires_at timestamptz NOT NULL);
CREATE INDEX forum_media_attempts_expiry_idx ON forum_media_attempts(expires_at);
