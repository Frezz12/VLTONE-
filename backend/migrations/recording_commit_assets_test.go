package migrations

import (
	"strings"
	"testing"
)

func TestRecordingCommitMigrationChecksClipAssets(t *testing.T) {
	body, err := Files.ReadFile("000013_recording_commit_assets.up.sql")
	if err != nil {
		t.Fatal(err)
	}
	sql := string(body)
	for _, required := range []string{
		"command_kind = 'clip.setAsset'",
		"jsonb_typeof(command_payload -> 'asset') = 'null'",
		"command_payload #>> '{asset,assetId}'",
		"command_payload #>> '{asset,sha256}'",
		"command_payload #>> '{asset,byteSize}'",
		"assets.kind IN ('audio', 'sample')",
		"blobs.kind = assets.kind",
		"blobs.status = 'ready'",
	} {
		if !strings.Contains(sql, required) {
			t.Fatalf("recording commit migration is missing %q", required)
		}
	}
}
