CREATE OR REPLACE FUNCTION collaboration_command_assets_ready(
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
