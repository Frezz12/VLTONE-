CREATE INDEX IF NOT EXISTS token_ledgers_kind_created_at_idx
    ON token_ledgers (kind, created_at);
CREATE INDEX IF NOT EXISTS token_reservations_unsettled_created_at_idx
    ON token_reservations (created_at) WHERE settled_at IS NULL;
