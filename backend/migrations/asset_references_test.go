package migrations

import (
	"strings"
	"testing"
)

func TestAssetReferenceMigrationDefinesExactReachabilityRoots(t *testing.T) {
	body, err := Files.ReadFile("000016_collaboration_asset_references.up.sql")
	if err != nil {
		t.Fatal(err)
	}
	sql := string(body)
	for _, required := range []string{
		"CREATE TABLE project_operation_assets",
		"REFERENCES project_ops(project_id, seq) ON DELETE CASCADE",
		"CREATE TABLE project_snapshot_assets",
		"REFERENCES project_snapshots(id, project_id) ON DELETE CASCADE",
		"jsonb_array_elements_text(snapshots.asset_ids)",
		"projects.head_seq > projects.snapshot_seq",
	} {
		if !strings.Contains(sql, required) {
			t.Fatalf("asset reference migration is missing %q", required)
		}
	}
}
