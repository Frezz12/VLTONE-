package collab

import (
	"context"
	"fmt"
	"os"
	"slices"
	"testing"
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"

	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
)

func TestPostgresSnapshotRetentionAndStorageMaintenance(t *testing.T) {
	dsn := os.Getenv("VLT_COLLAB_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("set VLT_COLLAB_TEST_DATABASE_URL to run collaboration PostgreSQL tests")
	}
	db, err := database.Open(dsn, false)
	if err != nil {
		t.Fatal(err)
	}
	sqlDB, _ := db.DB()
	t.Cleanup(func() { _ = sqlDB.Close() })
	tx := db.Begin()
	if tx.Error != nil {
		t.Fatal(tx.Error)
	}
	t.Cleanup(func() { _ = tx.Rollback().Error })

	now := time.Date(2026, 8, 30, 12, 0, 0, 0, time.UTC)
	suffix := uuid.NewString()
	owner := model.User{
		ID: uuid.New(), Email: "storage-" + suffix + "@example.test",
		EmailKey: "storage-" + suffix + "@example.test", Nickname: "storage-" + suffix,
		NicknameKey: "storage-" + suffix, PasswordHash: "unused", Locale: "en",
		Status: model.UserActive, ConsentVersion: "2026-08-23",
		ConsentAcceptedAt: now, ConsentIP: "127.0.0.1", CreatedAt: now, UpdatedAt: now,
	}
	if err := tx.Create(&owner).Error; err != nil {
		t.Fatal(err)
	}
	project := model.CloudProject{
		ID: uuid.New(), OwnerUserID: owner.ID, Title: "Storage maintenance",
		Status: model.ProjectActive, FormatVersion: CollaborationProjectFormatVersion,
		EngineVersion: "engine-test", MinimumAppVersion: "1.0.0",
		HeadSeq: 3, SnapshotSeq: 3, CreatedAt: now, UpdatedAt: now,
	}
	if err := tx.Create(&project).Error; err != nil {
		t.Fatal(err)
	}

	for seq := int64(1); seq <= project.HeadSeq; seq++ {
		operation := model.ProjectOperation{
			ProjectID: project.ID, Seq: seq, OpID: uuid.New(), Kind: "track.set_name",
			SchemaVersion: 1, BaseSeq: seq - 1,
			Payload:       datatypes.JSON([]byte(`{"trackId":"11111111-1111-4111-8111-111111111111","name":"x"}`)),
			Preconditions: datatypes.JSON([]byte(`[]`)),
			TouchedFields: datatypes.JSON([]byte(`[]`)),
			RequestHash:   fmt.Sprintf("%064x", seq), CreatedAt: now,
		}
		if err := tx.Create(&operation).Error; err != nil {
			t.Fatal(err)
		}
	}

	snapshotBlobs := make([]model.Blob, 3)
	snapshots := make([]model.ProjectSnapshot, 3)
	for index := range snapshotBlobs {
		verified := now
		snapshotBlobs[index] = model.Blob{
			ID: uuid.New(), SHA256: fmt.Sprintf("%064x", 100+index), Bytes: 10,
			ContentType: "application/vnd.vltone.project+json", Kind: "project_snapshot",
			ObjectKey: fmt.Sprintf("blobs/snapshot-%d", index), Status: BlobReady,
			CreatedAt: now.Add(time.Duration(index) * time.Second), VerifiedAt: &verified,
		}
		if err := tx.Create(&snapshotBlobs[index]).Error; err != nil {
			t.Fatal(err)
		}
		snapshots[index] = model.ProjectSnapshot{
			ID: uuid.New(), ProjectID: project.ID, Seq: int64(index + 1),
			BlobID: snapshotBlobs[index].ID, SchemaVersion: 6,
			CreatedAt: now.Add(time.Duration(index) * time.Second),
		}
		if err := tx.Create(&snapshots[index]).Error; err != nil {
			t.Fatal(err)
		}
	}
	endedAt := now
	startedAt := now.Add(-time.Minute)
	session := model.ProjectSession{
		ID: uuid.New(), ProjectID: project.ID, Mode: model.SessionModeIndependent,
		Status: model.ProjectSessionEnded, Version: 2, CreatedAt: startedAt,
		StartedAt: &startedAt, UpdatedAt: now, EndedAt: &endedAt,
	}
	if err := tx.Create(&session).Error; err != nil {
		t.Fatal(err)
	}
	completedAt := now
	request := model.ProjectSnapshotRequest{
		ID: uuid.New(), ProjectID: project.ID, SessionID: session.ID,
		TargetSeq: snapshots[0].Seq, Reason: model.SnapshotReasonAutosave,
		Status: model.SnapshotRequestCompleted, RequestedAt: now,
		NextRetryAt: now, CompletedSnapshotID: &snapshots[0].ID,
		CompletedAt: &completedAt,
	}
	if err := tx.Create(&request).Error; err != nil {
		t.Fatal(err)
	}
	if err := retainLatestSnapshotsTx(tx, project.ID, 2); err != nil {
		t.Fatal(err)
	}
	var snapshotCount, operationCount, oldRequestCount int64
	if err := tx.Model(&model.ProjectSnapshot{}).Where("project_id = ?", project.ID).
		Count(&snapshotCount).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Model(&model.ProjectOperation{}).Where("project_id = ?", project.ID).
		Count(&operationCount).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Model(&model.ProjectSnapshotRequest{}).Where("id = ?", request.ID).
		Count(&oldRequestCount).Error; err != nil {
		t.Fatal(err)
	}
	if snapshotCount != 2 || operationCount != 3 || oldRequestCount != 0 {
		t.Fatalf("retention snapshots=%d ops=%d oldRequests=%d",
			snapshotCount, operationCount, oldRequestCount)
	}

	assetID := uuid.New()
	partSize, partCount := int64(5<<20), 2
	upload := model.UploadSession{
		ID: uuid.New(), ProjectID: project.ID, AssetID: &assetID,
		CreatedBy: owner.ID, ExpectedSHA256: fmt.Sprintf("%064x", 200),
		ExpectedBytes: partSize * int64(partCount), Kind: "audio",
		ContentType: "audio/wav", DisplayName: "", RequestHash: fmt.Sprintf("%064x", 201),
		ProviderUploadID: "provider-upload", UploadMode: UploadModeMultipart,
		MultipartPartSize: &partSize, MultipartPartCount: &partCount,
		MultipartState: MultipartStateOpen, MultipartManifest: datatypes.JSON([]byte(`[]`)),
		ObjectKey: "uploads/project/upload", Status: UploadUploading,
		ExpiresAt: now.Add(-time.Minute), CreatedAt: now.Add(-time.Hour),
	}
	if err := tx.Create(&upload).Error; err != nil {
		t.Fatal(err)
	}
	fake := &maintenanceFakeStore{}
	assets := NewAssetService(tx, fake, time.Minute, time.Minute, time.Hour,
		1<<30, 64<<20, 16<<20, 100)
	assets.Now = func() time.Time { return now }
	removedUploads, err := assets.ReapExpiredUploads(context.Background(), 8)
	if err != nil || removedUploads != 1 {
		t.Fatalf("expired upload cleanup removed=%d err=%v", removedUploads, err)
	}
	if !slices.Contains(fake.aborts, upload.ObjectKey+":"+upload.ProviderUploadID) ||
		!slices.Contains(fake.deletes, upload.ObjectKey) {
		t.Fatalf("provider cleanup missing: aborts=%v deletes=%v", fake.aborts, fake.deletes)
	}

	oldUnreferenced := now.Add(-2 * time.Hour)
	oldBlob := model.Blob{
		ID: uuid.New(), SHA256: fmt.Sprintf("%064x", 300), Bytes: 1,
		ContentType: "audio/wav", Kind: "audio", ObjectKey: "blobs/old",
		Status: BlobReady, CreatedAt: now.Add(-3 * time.Hour), VerifiedAt: &now,
		UnreferencedAt: &oldUnreferenced,
	}
	referencedBlob := model.Blob{
		ID: uuid.New(), SHA256: fmt.Sprintf("%064x", 301), Bytes: 1,
		ContentType: "audio/wav", Kind: "audio", ObjectKey: "blobs/referenced",
		Status: BlobReady, CreatedAt: now.Add(-3 * time.Hour), VerifiedAt: &now,
		UnreferencedAt: &oldUnreferenced,
	}
	freshBlob := model.Blob{
		ID: uuid.New(), SHA256: fmt.Sprintf("%064x", 302), Bytes: 1,
		ContentType: "audio/wav", Kind: "audio", ObjectKey: "blobs/fresh",
		Status: BlobReady, CreatedAt: now, VerifiedAt: &now,
	}
	if err := tx.Create(&[]model.Blob{oldBlob, referencedBlob, freshBlob}).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Create(&model.ProjectAsset{
		ProjectID: project.ID, AssetID: uuid.New(), BlobID: referencedBlob.ID,
		Kind: "audio", CreatedAt: now,
	}).Error; err != nil {
		t.Fatal(err)
	}
	marked, err := assets.RefreshUnreferencedBlobs(context.Background(), 8)
	if err != nil || marked < 1 {
		t.Fatalf("unreferenced refresh marked=%d err=%v", marked, err)
	}
	deleted, err := assets.GarbageCollectUnreferencedBlobs(context.Background(),
		time.Hour, 8)
	if err != nil || deleted != 1 {
		t.Fatalf("blob GC deleted=%d err=%v", deleted, err)
	}
	if !slices.Contains(fake.deletes, oldBlob.ObjectKey) {
		t.Fatalf("unreferenced provider object was not deleted: %v", fake.deletes)
	}
	var oldCount, referencedCount int64
	_ = tx.Model(&model.Blob{}).Where("id = ?", oldBlob.ID).Count(&oldCount).Error
	_ = tx.Model(&model.Blob{}).Where("id = ?", referencedBlob.ID).Count(&referencedCount).Error
	if oldCount != 0 || referencedCount != 1 {
		t.Fatalf("GC reachability old=%d referenced=%d", oldCount, referencedCount)
	}
}
