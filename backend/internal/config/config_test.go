package config

import (
	"encoding/base64"
	"strings"
	"testing"

	"github.com/google/uuid"
)

func TestProductionRequiresSMTPAndGlobalBudget(t *testing.T) {
	t.Setenv("APP_ENV", "production")
	t.Setenv("PUBLIC_ORIGIN", "https://vlt.example")
	t.Setenv("ADMIN_ORIGIN", "https://admin.vlt.example")
	t.Setenv("DESKTOP_API_ORIGIN", "https://api.vlt.example")
	t.Setenv("AUTH_SIGNING_SEED", base64.StdEncoding.EncodeToString(make([]byte, 32)))
	t.Setenv("AI_GLOBAL_MONTHLY_TOKEN_LIMIT", "100000000")
	t.Setenv("SMTP_HOST", "")
	if _, err := Load(); err == nil || !strings.Contains(err.Error(), "SMTP_HOST") {
		t.Fatalf("production without SMTP was accepted: %v", err)
	}
	t.Setenv("SMTP_HOST", "smtp.vlt.example")
	if _, err := Load(); err != nil {
		t.Fatalf("valid production configuration was rejected: %v", err)
	}
}

func TestAICredentialsKeyMustBe32Bytes(t *testing.T) {
	t.Setenv("APP_ENV", "development")
	t.Setenv("AI_CREDENTIALS_KEY", base64.StdEncoding.EncodeToString([]byte("too short")))
	if _, err := Load(); err == nil || !strings.Contains(err.Error(), "AI_CREDENTIALS_KEY") {
		t.Fatalf("invalid AI credential key was accepted: %v", err)
	}

	t.Setenv("AI_CREDENTIALS_KEY", base64.StdEncoding.EncodeToString(make([]byte, 32)))
	if loaded, err := Load(); err != nil || len(loaded.AICredentialsKey) != 32 {
		t.Fatalf("valid AI credential key was rejected: key=%d err=%v", len(loaded.AICredentialsKey), err)
	}
}

func TestCollaborationDefaultsToDisabledAndBoundsLimits(t *testing.T) {
	t.Setenv("APP_ENV", "development")
	t.Setenv("COLLABORATION_ENABLED", "")
	t.Setenv("COLLAB_MAX_PARTICIPANTS", "99")
	t.Setenv("COLLAB_SNAPSHOT_OPS", "0")
	t.Setenv("COLLAB_LEASE_SECONDS", "2")
	t.Setenv("COLLAB_MEMBER_STALE_SECONDS", "2")
	t.Setenv("COLLAB_REAPER_BATCH", "0")
	t.Setenv("COLLAB_STORAGE_REAPER_BATCH", "0")
	t.Setenv("COLLAB_MAINTENANCE_TIMEOUT_SECONDS", "1")
	t.Setenv("COLLAB_BLOB_RETENTION_SECONDS", "60")
	t.Setenv("COLLAB_WS_DB_TIMEOUT_SECONDS", "300")
	t.Setenv("COLLAB_ROOM_QUEUE_BYTES", "1")
	t.Setenv("COLLAB_MULTIPART_THRESHOLD_BYTES", "1")
	t.Setenv("COLLAB_MULTIPART_PART_BYTES", "1")
	t.Setenv("COLLAB_MULTIPART_URL_BATCH", "5000")

	loaded, err := Load()
	if err != nil {
		t.Fatalf("development configuration was rejected: %v", err)
	}
	if loaded.CollaborationEnabled {
		t.Fatal("collaboration must remain opt-in until release gates pass")
	}
	if loaded.CollabMaxParticipants != 8 {
		t.Fatalf("participant limit escaped v1 bounds: %d", loaded.CollabMaxParticipants)
	}
	if loaded.CollabSnapshotOps != 500 {
		t.Fatalf("invalid snapshot threshold did not use its default: %d", loaded.CollabSnapshotOps)
	}
	if loaded.CollabLeaseSeconds != 15 {
		t.Fatalf("invalid lease duration did not use its default: %d", loaded.CollabLeaseSeconds)
	}
	if loaded.CollabMemberStaleSeconds != 45 || loaded.CollabReaperBatch != 256 ||
		loaded.CollabStorageReaperBatch != 128 ||
		loaded.CollabMaintenanceTimeoutSeconds != 20 ||
		loaded.CollabBlobRetentionSeconds != 604800 ||
		loaded.CollabWSDBTimeoutSeconds != 15 || loaded.CollabRoomQueueBytes != 8<<20 {
		t.Fatalf("invalid collaboration runtime limits did not use safe defaults: %+v", loaded)
	}
	if loaded.CollabMultipartThresholdBytes != 64<<20 ||
		loaded.CollabMultipartPartBytes != 16<<20 || loaded.CollabMultipartURLBatch != 100 {
		t.Fatalf("invalid multipart limits did not use safe defaults: %+v", loaded)
	}
	t.Setenv("COLLAB_MULTIPART_THRESHOLD_BYTES", "5368709121")
	loaded, err = Load()
	if err != nil || loaded.CollabMultipartThresholdBytes != 64<<20 {
		t.Fatalf("threshold above the S3 single-PUT limit was accepted: threshold=%d err=%v",
			loaded.CollabMultipartThresholdBytes, err)
	}

	t.Setenv("COLLABORATION_ENABLED", "true")
	t.Setenv("COLLAB_MAX_PARTICIPANTS", "4")
	t.Setenv("COLLAB_SNAPSHOT_OPS", "250")
	t.Setenv("COLLAB_MEMBER_STALE_SECONDS", "90")
	t.Setenv("COLLAB_REAPER_BATCH", "100")
	t.Setenv("COLLAB_STORAGE_REAPER_BATCH", "64")
	t.Setenv("COLLAB_MAINTENANCE_TIMEOUT_SECONDS", "30")
	t.Setenv("COLLAB_BLOB_RETENTION_SECONDS", "172800")
	t.Setenv("COLLAB_WS_DB_TIMEOUT_SECONDS", "20")
	t.Setenv("COLLAB_ROOM_QUEUE_BYTES", "16777216")
	t.Setenv("COLLAB_OBJECT_ENDPOINT", "http://127.0.0.1:9000")
	t.Setenv("COLLAB_OBJECT_BUCKET", "vlt-collaboration")
	t.Setenv("COLLAB_OBJECT_ACCESS_KEY_ID", "development-access")
	t.Setenv("COLLAB_OBJECT_SECRET_ACCESS_KEY", "development-secret")
	t.Setenv("COLLAB_MULTIPART_THRESHOLD_BYTES", "33554432")
	t.Setenv("COLLAB_MULTIPART_PART_BYTES", "8388608")
	t.Setenv("COLLAB_MULTIPART_URL_BATCH", "40")
	loaded, err = Load()
	if err != nil {
		t.Fatalf("valid collaboration configuration was rejected: %v", err)
	}
	if !loaded.CollaborationEnabled || loaded.CollabMaxParticipants != 4 || loaded.CollabSnapshotOps != 250 ||
		loaded.CollabMemberStaleSeconds != 90 || loaded.CollabReaperBatch != 100 ||
		loaded.CollabStorageReaperBatch != 64 ||
		loaded.CollabMaintenanceTimeoutSeconds != 30 ||
		loaded.CollabBlobRetentionSeconds != 172800 ||
		loaded.CollabWSDBTimeoutSeconds != 20 || loaded.CollabRoomQueueBytes != 16777216 ||
		loaded.CollabMultipartThresholdBytes != 33554432 ||
		loaded.CollabMultipartPartBytes != 8388608 || loaded.CollabMultipartURLBatch != 40 {
		t.Fatalf("valid collaboration values were not loaded: %+v", loaded)
	}
}

func TestCollaborationStorageConfigurationIsFailClosed(t *testing.T) {
	t.Setenv("APP_ENV", "development")
	t.Setenv("COLLABORATION_ENABLED", "true")
	t.Setenv("COLLAB_OBJECT_ENDPOINT", "")
	t.Setenv("COLLAB_OBJECT_BUCKET", "")
	t.Setenv("COLLAB_OBJECT_ACCESS_KEY_ID", "")
	t.Setenv("COLLAB_OBJECT_SECRET_ACCESS_KEY", "")
	if _, err := Load(); err == nil || !strings.Contains(err.Error(), "COLLAB_OBJECT_ENDPOINT") {
		t.Fatalf("collaboration without object storage was accepted: %v", err)
	}

	t.Setenv("COLLAB_OBJECT_ENDPOINT", "http://127.0.0.1:9000")
	t.Setenv("COLLAB_OBJECT_BUCKET", "vlt-collaboration")
	t.Setenv("COLLAB_OBJECT_ACCESS_KEY_ID", "access")
	t.Setenv("COLLAB_OBJECT_SECRET_ACCESS_KEY", "secret")
	if _, err := Load(); err != nil {
		t.Fatalf("valid development object storage was rejected: %v", err)
	}

	t.Setenv("APP_ENV", "production")
	t.Setenv("PUBLIC_ORIGIN", "https://vlt.example")
	t.Setenv("ADMIN_ORIGIN", "https://admin.vlt.example")
	t.Setenv("DESKTOP_API_ORIGIN", "https://api.vlt.example")
	t.Setenv("AUTH_SIGNING_SEED", base64.StdEncoding.EncodeToString(make([]byte, 32)))
	t.Setenv("AI_GLOBAL_MONTHLY_TOKEN_LIMIT", "100000000")
	t.Setenv("SMTP_HOST", "smtp.vlt.example")
	if _, err := Load(); err == nil || !strings.Contains(err.Error(), "HTTPS") {
		t.Fatalf("production HTTP object endpoint was accepted: %v", err)
	}

	t.Setenv("COLLAB_OBJECT_ENDPOINT", "https://objects.vlt.example")
	if _, err := Load(); err != nil {
		t.Fatalf("valid production object storage was rejected: %v", err)
	}
}

func TestCollaborationAllowlistIsDefaultDenyAndValidatesUUIDs(t *testing.T) {
	t.Setenv("APP_ENV", "development")
	t.Setenv("COLLAB_ALLOWED_USER_IDS", "")
	loaded, err := Load()
	if err != nil || len(loaded.CollabAllowedUserIDs) != 0 {
		t.Fatalf("empty allowlist was not default-deny: ids=%v err=%v",
			loaded.CollabAllowedUserIDs, err)
	}
	first, second := uuid.New(), uuid.New()
	t.Setenv("COLLAB_ALLOWED_USER_IDS", first.String()+", "+second.String())
	loaded, err = Load()
	if err != nil || len(loaded.CollabAllowedUserIDs) != 2 ||
		loaded.CollabAllowedUserIDs[0] != first || loaded.CollabAllowedUserIDs[1] != second {
		t.Fatalf("valid allowlist was not parsed: ids=%v err=%v",
			loaded.CollabAllowedUserIDs, err)
	}
	t.Setenv("COLLAB_ALLOWED_USER_IDS", "not-a-uuid")
	if _, err := Load(); err == nil || !strings.Contains(err.Error(), "COLLAB_ALLOWED_USER_IDS") {
		t.Fatalf("invalid collaboration allowlist was accepted: %v", err)
	}
}

func TestCloudRecordingCannotBeEnabledInV1(t *testing.T) {
	t.Setenv("APP_ENV", "development")
	t.Setenv("COLLAB_RECORDING_ENABLED", "true")
	if _, err := Load(); err == nil || !strings.Contains(err.Error(), "must remain false") {
		t.Fatalf("cloud recording was enabled in V1: %v", err)
	}
}
