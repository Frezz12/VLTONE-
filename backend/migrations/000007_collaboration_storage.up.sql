ALTER TABLE upload_sessions
    ADD COLUMN asset_id uuid,
    ADD COLUMN snapshot_seq bigint CHECK (snapshot_seq >= 0),
    ADD COLUMN snapshot_schema_version integer CHECK (snapshot_schema_version > 0),
    ADD COLUMN content_type text NOT NULL DEFAULT 'application/octet-stream'
        CHECK (char_length(content_type) BETWEEN 1 AND 160),
    ADD COLUMN display_name text NOT NULL DEFAULT '' CHECK (char_length(display_name) <= 255),
    ADD COLUMN request_hash text;

-- Collaboration storage was feature-gated before this migration, but keep the
-- upgrade safe for dogfood databases that prepared an upload with the initial
-- table shape. Those unverifiable sessions are made non-resumable and receive
-- structurally valid purpose metadata; no blob or asset is linked.
UPDATE upload_sessions
SET status = 'aborted', asset_id = id, request_hash = repeat('0', 64)
WHERE kind <> 'project_snapshot';
UPDATE upload_sessions AS uploads
SET status = 'aborted',
    snapshot_seq = projects.head_seq,
    snapshot_schema_version = 6,
    request_hash = repeat('0', 64)
FROM cloud_projects AS projects
WHERE uploads.kind = 'project_snapshot' AND projects.id = uploads.project_id;

ALTER TABLE upload_sessions
    ALTER COLUMN request_hash SET NOT NULL,
    ADD CONSTRAINT upload_sessions_request_hash_check
        CHECK (request_hash ~ '^[0-9a-f]{64}$'),
    ADD CONSTRAINT upload_sessions_purpose_check CHECK (
        (kind = 'project_snapshot' AND asset_id IS NULL AND snapshot_seq IS NOT NULL
            AND snapshot_schema_version IS NOT NULL AND display_name = '')
        OR
        (kind <> 'project_snapshot' AND asset_id IS NOT NULL AND snapshot_seq IS NULL
            AND snapshot_schema_version IS NULL)
    );

CREATE INDEX upload_sessions_project_asset_idx
    ON upload_sessions(project_id, asset_id) WHERE asset_id IS NOT NULL;
CREATE INDEX upload_sessions_project_snapshot_idx
    ON upload_sessions(project_id, snapshot_seq) WHERE snapshot_seq IS NOT NULL;

-- The operation sequencer remains authoritative even if a future HTTP handler
-- accidentally omits an application-level readiness check. Supported v1
-- commands may reference an asset only after that exact assetId/hash/size is
-- linked to a server-verified ready blob in the same project.
CREATE FUNCTION collaboration_command_assets_ready(
    checked_project_id uuid,
    command_kind text,
    command_payload jsonb
) RETURNS boolean
LANGUAGE plpgsql
STABLE
AS $$
DECLARE
    nested_command jsonb;
    referenced_asset_id uuid;
    referenced_sha256 text;
    referenced_bytes bigint;
BEGIN
    IF command_kind = 'take.add' THEN
        BEGIN
            referenced_asset_id := (command_payload #>> '{take,asset,assetId}')::uuid;
            referenced_sha256 := command_payload #>> '{take,asset,sha256}';
            referenced_bytes := (command_payload #>> '{take,asset,byteSize}')::bigint;
        EXCEPTION WHEN invalid_text_representation OR numeric_value_out_of_range THEN
            RETURN false;
        END;
        IF referenced_asset_id IS NULL OR referenced_sha256 IS NULL OR referenced_bytes IS NULL THEN
            RETURN false;
        END IF;
        RETURN EXISTS (
            SELECT 1
            FROM project_assets AS assets
            JOIN blobs ON blobs.id = assets.blob_id
            WHERE assets.project_id = checked_project_id
              AND assets.asset_id = referenced_asset_id
              AND assets.kind = 'audio'
              AND blobs.status = 'ready'
              AND blobs.sha256 = referenced_sha256
              AND blobs.bytes = referenced_bytes
        );
    ELSIF command_kind = 'batch' THEN
        IF jsonb_typeof(command_payload -> 'commands') <> 'array' THEN
            RETURN false;
        END IF;
        FOR nested_command IN SELECT value FROM jsonb_array_elements(command_payload -> 'commands') LOOP
            IF NOT collaboration_command_assets_ready(
                checked_project_id,
                nested_command ->> 'kind',
                nested_command -> 'payload'
            ) THEN
                RETURN false;
            END IF;
        END LOOP;
    END IF;
    RETURN true;
END;
$$;

CREATE FUNCTION enforce_collaboration_command_assets_ready()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    IF NOT collaboration_command_assets_ready(NEW.project_id, NEW.kind, NEW.payload) THEN
        RAISE EXCEPTION 'project operation references an unavailable asset'
            USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER project_ops_assets_ready
    BEFORE INSERT ON project_ops
    FOR EACH ROW EXECUTE FUNCTION enforce_collaboration_command_assets_ready();
