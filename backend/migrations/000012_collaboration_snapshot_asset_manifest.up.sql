-- A snapshot declares the exact immutable project assets referenced by its
-- canonical document. Manifests are normalized as strictly increasing,
-- canonical lowercase UUID strings so their JSON representation is stable for
-- idempotency and bounded before any readiness query runs.
CREATE FUNCTION collaboration_snapshot_asset_manifest_valid(manifest jsonb)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE
STRICT
PARALLEL SAFE
AS $$
DECLARE
    item jsonb;
    asset_id_text text;
    previous_asset_id text;
    parsed_asset_id uuid;
BEGIN
    IF jsonb_typeof(manifest) <> 'array'
        OR jsonb_array_length(manifest) > 100000 THEN
        RETURN false;
    END IF;

    FOR item IN SELECT value FROM jsonb_array_elements(manifest)
    LOOP
        IF jsonb_typeof(item) <> 'string' THEN
            RETURN false;
        END IF;
        asset_id_text := item #>> '{}';
        IF asset_id_text !~ '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' THEN
            RETURN false;
        END IF;
        BEGIN
            parsed_asset_id := asset_id_text::uuid;
        EXCEPTION WHEN invalid_text_representation THEN
            RETURN false;
        END;
        IF parsed_asset_id = '00000000-0000-0000-0000-000000000000'::uuid
            OR parsed_asset_id::text <> asset_id_text
            OR (previous_asset_id IS NOT NULL AND previous_asset_id >= asset_id_text) THEN
            RETURN false;
        END IF;
        previous_asset_id := asset_id_text;
    END LOOP;
    RETURN true;
END;
$$;

ALTER TABLE upload_sessions
    ADD COLUMN snapshot_asset_ids jsonb NOT NULL DEFAULT '[]'::jsonb,
    ADD CONSTRAINT upload_sessions_snapshot_asset_ids_check CHECK (
        collaboration_snapshot_asset_manifest_valid(snapshot_asset_ids)
        AND (kind = 'project_snapshot' OR snapshot_asset_ids = '[]'::jsonb)
    );

ALTER TABLE project_snapshots
    ADD COLUMN asset_ids jsonb NOT NULL DEFAULT '[]'::jsonb,
    ADD CONSTRAINT project_snapshots_asset_ids_check
        CHECK (collaboration_snapshot_asset_manifest_valid(asset_ids));

COMMENT ON COLUMN upload_sessions.snapshot_asset_ids IS
    'Normalized exact asset manifest bound to snapshot upload idempotency; empty for non-snapshot uploads.';
COMMENT ON COLUMN project_snapshots.asset_ids IS
    'Normalized exact asset UUID manifest declared by the canonical snapshot; clients compare it with parsed AssetRefs.';
