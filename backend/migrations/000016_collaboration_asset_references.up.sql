-- Asset reachability is defined by retained snapshots and operations newer
-- than the last materialized snapshot. project_assets remains the immutable
-- project-local catalog used by downloads and idempotent upload completion;
-- it is not, by itself, a durable reachability root.
ALTER TABLE project_snapshots
    ADD CONSTRAINT project_snapshots_id_project_unique UNIQUE (id, project_id);

CREATE TABLE project_operation_assets (
    project_id uuid NOT NULL,
    operation_seq bigint NOT NULL,
    asset_id uuid NOT NULL,
    PRIMARY KEY (project_id, operation_seq, asset_id),
    FOREIGN KEY (project_id, operation_seq)
        REFERENCES project_ops(project_id, seq) ON DELETE CASCADE,
    FOREIGN KEY (project_id, asset_id)
        REFERENCES project_assets(project_id, asset_id) ON DELETE CASCADE
);
CREATE INDEX project_operation_assets_asset_idx
    ON project_operation_assets(project_id, asset_id);

CREATE TABLE project_snapshot_assets (
    snapshot_id uuid NOT NULL,
    project_id uuid NOT NULL,
    asset_id uuid NOT NULL,
    PRIMARY KEY (snapshot_id, asset_id),
    FOREIGN KEY (snapshot_id, project_id)
        REFERENCES project_snapshots(id, project_id) ON DELETE CASCADE,
    FOREIGN KEY (project_id, asset_id)
        REFERENCES project_assets(project_id, asset_id) ON DELETE CASCADE
);
CREATE INDEX project_snapshot_assets_asset_idx
    ON project_snapshot_assets(project_id, asset_id);

-- Snapshot manifests already contain exact canonical asset IDs.
INSERT INTO project_snapshot_assets(snapshot_id, project_id, asset_id)
SELECT snapshots.id, snapshots.project_id, assets.asset_id
FROM project_snapshots AS snapshots
CROSS JOIN LATERAL jsonb_array_elements_text(snapshots.asset_ids) AS manifest(asset_id)
JOIN project_assets AS assets
  ON assets.project_id = snapshots.project_id
 AND assets.asset_id = manifest.asset_id::uuid
ON CONFLICT DO NOTHING;

-- Pre-v2 command payloads are intentionally not decoded in SQL. If a legacy
-- project has operations after its last snapshot, conservatively pin its
-- catalog at the current head. The next exact snapshot removes these refs.
INSERT INTO project_operation_assets(project_id, operation_seq, asset_id)
SELECT projects.id, projects.head_seq, assets.asset_id
FROM cloud_projects AS projects
JOIN project_ops AS operations
  ON operations.project_id = projects.id AND operations.seq = projects.head_seq
JOIN project_assets AS assets ON assets.project_id = projects.id
WHERE projects.head_seq > projects.snapshot_seq
ON CONFLICT DO NOTHING;

COMMENT ON TABLE project_operation_assets IS
    'Exact assets introduced by durable operations newer than the latest accepted snapshot.';
COMMENT ON TABLE project_snapshot_assets IS
    'Exact assets reachable from retained canonical snapshot manifests.';
COMMENT ON COLUMN blobs.unreferenced_at IS
    'First observed time with no retained snapshot or post-snapshot operation root; reset whenever an exact root is created.';
COMMENT ON COLUMN project_ops.touched_fields IS
    'Canonical field-head keys re-derived and verified by the server from the v2 command kind and payload.';
