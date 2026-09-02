package collab

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"

	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/objectstore"
)

// publicationObjectStore models only the immutable verification boundary used
// by AssetService. Tests explicitly stage provider-observed metadata after a
// prepare call; VerifyAndPromote refuses missing or mismatched objects instead
// of trusting the upload request.
type publicationObjectStore struct {
	staged      map[string]objectstore.ExpectedObject
	promoted    map[string]objectstore.ExpectedObject
	verifyCalls int
	deleteErr   error
	deletes     []string
}

func newPublicationObjectStore() *publicationObjectStore {
	return &publicationObjectStore{
		staged:   make(map[string]objectstore.ExpectedObject),
		promoted: make(map[string]objectstore.ExpectedObject),
	}
}

func (s *publicationObjectStore) stage(key string, value objectstore.ExpectedObject) {
	s.staged[key] = value
}

func (s *publicationObjectStore) PresignPut(_ context.Context, key string,
	expected objectstore.ExpectedObject, ttl time.Duration) (objectstore.PresignedRequest, error) {
	return objectstore.PresignedRequest{
		Method: "PUT", URL: "https://object-store.invalid/" + key,
		Headers:   map[string]string{"Content-Type": expected.ContentType},
		ExpiresAt: time.Now().Add(ttl),
	}, nil
}

func (s *publicationObjectStore) PresignGet(_ context.Context, key string,
	ttl time.Duration) (objectstore.PresignedRequest, error) {
	if _, ok := s.promoted[key]; !ok {
		return objectstore.PresignedRequest{}, objectstore.ErrObjectNotFound
	}
	return objectstore.PresignedRequest{
		Method: "GET", URL: "https://object-store.invalid/" + key,
		ExpiresAt: time.Now().Add(ttl),
	}, nil
}

func (*publicationObjectStore) CreateMultipart(context.Context, string,
	objectstore.ExpectedObject) (string, error) {
	return "", errors.New("unexpected multipart create")
}

func (*publicationObjectStore) PresignMultipartPart(context.Context, string,
	string, int, int64, time.Duration) (objectstore.PresignedRequest, error) {
	return objectstore.PresignedRequest{}, errors.New("unexpected multipart presign")
}

func (*publicationObjectStore) ListMultipartParts(context.Context, string,
	string, int) ([]objectstore.UploadedPart, error) {
	return nil, errors.New("unexpected multipart list")
}

func (*publicationObjectStore) CompleteMultipart(context.Context, string,
	string, []objectstore.UploadedPart) error {
	return errors.New("unexpected multipart completion")
}

func (*publicationObjectStore) AbortMultipart(context.Context, string, string) error {
	return errors.New("unexpected multipart abort")
}

func (s *publicationObjectStore) VerifyAndPromote(_ context.Context, stagingKey,
	finalKey string, expected objectstore.ExpectedObject) error {
	s.verifyCalls++
	observed, ok := s.staged[stagingKey]
	if !ok {
		return objectstore.ErrObjectNotFound
	}
	if observed != expected {
		return objectstore.ErrInvalidObject
	}
	s.promoted[finalKey] = observed
	return nil
}

func (s *publicationObjectStore) Delete(_ context.Context, key string) error {
	s.deletes = append(s.deletes, key)
	if s.deleteErr != nil {
		return s.deleteErr
	}
	delete(s.staged, key)
	return nil
}

type publicationActor struct {
	user    model.User
	device  model.Device
	session model.DesktopSession
}

func createPublicationActor(t *testing.T, tx *gorm.DB, now time.Time,
	prefix string) publicationActor {
	t.Helper()
	suffix := prefix + "-" + uuid.NewString()
	actor := publicationActor{
		user: model.User{
			ID: uuid.New(), Email: suffix + "@example.test",
			EmailKey: suffix + "@example.test", Nickname: suffix,
			NicknameKey: suffix, PasswordHash: "unused", Locale: "en",
			Status: model.UserActive, ConsentVersion: "2026-08-23",
			ConsentAcceptedAt: now, ConsentIP: "127.0.0.1",
			CreatedAt: now, UpdatedAt: now,
		},
	}
	if err := tx.Create(&actor.user).Error; err != nil {
		t.Fatal(err)
	}
	actor.device = model.Device{
		ID: uuid.New(), UserID: actor.user.ID, InstallID: uuid.NewString(),
		DisplayName: suffix, Platform: "macos", OSVersion: "test",
		AppVersion: "1.0.0", Hardware: datatypes.JSON([]byte(`{}`)),
		FirstSeenAt: now, LastSeenAt: now,
	}
	if err := tx.Create(&actor.device).Error; err != nil {
		t.Fatal(err)
	}
	actor.session = model.DesktopSession{
		ID: uuid.New(), UserID: actor.user.ID, DeviceID: actor.device.ID,
		RefreshTokenHash: uuid.NewString(), ReporterTokenHash: uuid.NewString(),
		ReporterExpiresAt: now.Add(72 * time.Hour),
		ExpiresAt:         now.Add(30 * 24 * time.Hour), CreatedAt: now, LastSeenAt: now,
	}
	if err := tx.Create(&actor.session).Error; err != nil {
		t.Fatal(err)
	}
	return actor
}

func digestBytes(value []byte) string {
	digest := sha256.Sum256(value)
	return fmt.Sprintf("%x", digest[:])
}

// This test uses the same opt-in convention as the other collaboration store
// integration tests. It never migrates or clears the database; all fixtures
// and lifecycle mutations are rolled back together.
func TestPostgresInitialPublicationLifecycle(t *testing.T) {
	dsn := os.Getenv("VLT_COLLAB_TEST_DATABASE_URL")
	if dsn == "" {
		if os.Getenv("CI") != "" {
			t.Fatal("VLT_COLLAB_TEST_DATABASE_URL is required in CI")
		}
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

	ctx := context.Background()
	now := time.Date(2026, 8, 30, 18, 0, 0, 0, time.UTC)
	owner := createPublicationActor(t, tx, now, "publish-owner")
	editor := createPublicationActor(t, tx, now, "publish-editor")
	store := NewStore(tx, 8)
	store.Now = func() time.Time { return now }
	objects := newPublicationObjectStore()
	assets := NewAssetService(tx, objects, time.Minute, time.Minute, time.Hour,
		1<<30, 64<<20, 16<<20, 100)
	assets.Now = func() time.Time { return now }

	created, err := store.CreateProject(ctx, CreateProjectInput{
		OwnerUserID: owner.user.ID, Title: "Initial publication",
		FormatVersion: CollaborationProjectFormatVersion,
		EngineVersion: "engine-test", MinimumAppVersion: "1.0.0",
	})
	if err != nil {
		t.Fatal(err)
	}
	projectID := created.Project.ID
	if created.Project.Status != model.ProjectUploading || created.Project.HeadSeq != 0 {
		t.Fatalf("new project is not an empty uploading project: %#v", created.Project)
	}
	inviter := owner.user.ID
	if err := tx.Create(&model.ProjectMember{
		ProjectID: projectID, UserID: editor.user.ID, Role: model.ProjectRoleEditor,
		ColorIndex: 1, InvitedBy: &inviter, JoinedAt: now, UpdatedAt: now,
	}).Error; err != nil {
		t.Fatal(err)
	}

	if _, err := store.CompletePublication(ctx, projectID, editor.user.ID); !errors.Is(err, ErrForbidden) {
		t.Fatalf("non-owner published a project: %v", err)
	}
	if _, err := store.CompletePublication(ctx, projectID, owner.user.ID); !errors.Is(err, ErrConflict) {
		t.Fatalf("publication without a canonical snapshot returned %v", err)
	}

	assetID, assetUploadID := uuid.New(), uuid.New()
	assetPayload := []byte("verified publication audio")
	assetExpected := objectstore.ExpectedObject{
		SHA256: digestBytes(assetPayload), Bytes: int64(len(assetPayload)),
		ContentType: "audio/wav",
	}
	assetPreparation, err := assets.PrepareAssetUpload(ctx, PrepareAssetUploadInput{
		ProjectID: projectID, UploadID: assetUploadID, AssetID: assetID,
		ActorUserID: owner.user.ID, DeviceID: owner.device.ID,
		ActorSessionID: owner.session.ID, SHA256: assetExpected.SHA256,
		Bytes: assetExpected.Bytes, Kind: "audio", ContentType: assetExpected.ContentType,
		DisplayName: "/private/source/take.wav",
	})
	if err != nil {
		t.Fatal(err)
	}
	if assetPreparation.AlreadyAvailable || assetPreparation.Request == nil ||
		assetPreparation.Status != UploadUploading {
		t.Fatalf("unexpected asset preparation: %#v", assetPreparation)
	}

	snapshotPayload := []byte(fmt.Sprintf(`{"formatVersion":7,"assetRefs":[{"assetId":%q}]}`,
		assetID.String()))
	snapshotExpected := objectstore.ExpectedObject{
		SHA256: digestBytes(snapshotPayload), Bytes: int64(len(snapshotPayload)),
		ContentType: "application/vnd.vltone.project+json",
	}
	snapshotInput := func(uploadID uuid.UUID, assetIDs []string) PrepareSnapshotUploadInput {
		return PrepareSnapshotUploadInput{
			ProjectID: projectID, UploadID: uploadID,
			ActorUserID: owner.user.ID, DeviceID: owner.device.ID,
			ActorSessionID: owner.session.ID, Seq: 0,
			SchemaVersion: CollaborationProjectFormatVersion,
			SHA256:        snapshotExpected.SHA256, Bytes: snapshotExpected.Bytes,
			ContentType: snapshotExpected.ContentType, AssetIDs: assetIDs,
		}
	}
	if _, err := assets.PrepareSnapshotUpload(ctx,
		snapshotInput(uuid.New(), []string{assetID.String()})); !errors.Is(err, ErrAssetUnavailable) {
		t.Fatalf("pending declared asset was accepted by snapshot prepare: %v", err)
	}
	if _, err := assets.PrepareSnapshotUpload(ctx,
		snapshotInput(uuid.New(), []string{uuid.NewString()})); !errors.Is(err, ErrAssetUnavailable) {
		t.Fatalf("missing declared asset was accepted by snapshot prepare: %v", err)
	}
	if _, err := assets.PrepareSnapshotUpload(ctx,
		snapshotInput(uuid.New(), []string{assetID.String(), assetID.String()})); !errors.Is(err, ErrValidation) {
		t.Fatalf("duplicate snapshot manifest identifiers returned %v", err)
	}

	foreignProject, err := store.CreateProject(ctx, CreateProjectInput{
		OwnerUserID: owner.user.ID, Title: "Foreign manifest asset",
		FormatVersion: CollaborationProjectFormatVersion,
		EngineVersion: "engine-test", MinimumAppVersion: "1.0.0",
	})
	if err != nil {
		t.Fatal(err)
	}
	verifiedAt := now
	foreignBlob := model.Blob{
		ID: uuid.New(), SHA256: digestBytes([]byte("foreign ready asset")), Bytes: 19,
		ContentType: "audio/wav", Kind: "audio", Status: BlobReady,
		ObjectKey: "blobs/foreign-manifest-asset", CreatedAt: now, VerifiedAt: &verifiedAt,
	}
	foreignAssetID := uuid.New()
	if err := tx.Create(&foreignBlob).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Create(&model.ProjectAsset{
		ProjectID: foreignProject.Project.ID, AssetID: foreignAssetID,
		BlobID: foreignBlob.ID, Kind: "audio", DisplayName: "foreign.wav", CreatedAt: now,
	}).Error; err != nil {
		t.Fatal(err)
	}
	if _, err := assets.PrepareSnapshotUpload(ctx,
		snapshotInput(uuid.New(), []string{foreignAssetID.String()})); !errors.Is(err, ErrAssetUnavailable) {
		t.Fatalf("foreign project asset was accepted by snapshot prepare: %v", err)
	}

	// A provider-observed checksum/size mismatch must not create a ready asset,
	// and therefore cannot satisfy the canonical snapshot manifest.
	mismatched := assetExpected
	mismatched.SHA256 = digestBytes([]byte("different provider object"))
	objects.stage(stagingObjectKey(projectID, assetUploadID), mismatched)
	if _, err := assets.CompleteAssetUpload(ctx, projectID, assetUploadID,
		owner.user.ID, owner.device.ID, owner.session.ID, nil); !errors.Is(err, objectstore.ErrInvalidObject) {
		t.Fatalf("mismatched provider asset returned %v", err)
	}
	objects.stage(stagingObjectKey(projectID, assetUploadID), assetExpected)
	objects.deleteErr = objectstore.ErrProvider
	completedAsset, err := assets.CompleteAssetUpload(ctx, projectID, assetUploadID,
		owner.user.ID, owner.device.ID, owner.session.ID, nil)
	if err != nil {
		t.Fatal(err)
	}
	if completedAsset.Asset.AssetID != assetID || completedAsset.Asset.Kind != "audio" ||
		completedAsset.Asset.DisplayName != "take.wav" ||
		completedAsset.Blob.Status != BlobReady ||
		completedAsset.Blob.SHA256 != assetExpected.SHA256 ||
		completedAsset.Blob.Bytes != assetExpected.Bytes {
		t.Fatalf("verified asset was not linked exactly: %#v", completedAsset)
	}
	var stagedCleanup model.ObjectCleanupJob
	if err := tx.Where("object_key = ?", stagingObjectKey(projectID, assetUploadID)).
		First(&stagedCleanup).Error; err != nil || stagedCleanup.Status != objectCleanupPending ||
		stagedCleanup.AttemptCount != 1 {
		t.Fatalf("asset completion did not retain failed staging cleanup: job=%#v err=%v",
			stagedCleanup, err)
	}
	objects.deleteErr = nil
	if err := tx.Model(&model.ObjectCleanupJob{}).Where("id = ?", stagedCleanup.ID).
		Update("retry_available_at", now).Error; err != nil {
		t.Fatal(err)
	}
	if cleaned, err := assets.DrainObjectCleanupJobs(ctx, 8); err != nil || cleaned != 1 {
		t.Fatalf("asset staging retry cleaned=%d err=%v", cleaned, err)
	}
	if err := tx.Model(&model.Blob{}).Where("id = ?", completedAsset.Blob.ID).
		Update("status", BlobQuarantined).Error; err != nil {
		t.Fatal(err)
	}
	if _, err := assets.PrepareSnapshotUpload(ctx,
		snapshotInput(uuid.New(), []string{assetID.String()})); !errors.Is(err, ErrAssetUnavailable) {
		t.Fatalf("snapshot prepare accepted an unready declared asset: %v", err)
	}
	if err := tx.Model(&model.Blob{}).Where("id = ?", completedAsset.Blob.ID).
		Updates(map[string]any{"status": BlobReady, "kind": "sample"}).Error; err != nil {
		t.Fatal(err)
	}
	if _, err := assets.PrepareSnapshotUpload(ctx,
		snapshotInput(uuid.New(), []string{assetID.String()})); !errors.Is(err, ErrAssetUnavailable) {
		t.Fatalf("snapshot prepare accepted mismatched asset/blob kinds: %v", err)
	}
	if err := tx.Model(&model.Blob{}).Where("id = ?", completedAsset.Blob.ID).
		Update("kind", "audio").Error; err != nil {
		t.Fatal(err)
	}

	snapshotUploadID := uuid.New()
	snapshotPreparation, err := assets.PrepareSnapshotUpload(ctx,
		snapshotInput(snapshotUploadID, []string{assetID.String()}))
	if err != nil {
		t.Fatal(err)
	}
	if snapshotPreparation.SnapshotSeq == nil || *snapshotPreparation.SnapshotSeq != 0 ||
		snapshotPreparation.Request == nil {
		t.Fatalf("unexpected snapshot preparation: %#v", snapshotPreparation)
	}
	if err := tx.Model(&model.Blob{}).Where("id = ?", completedAsset.Blob.ID).
		Update("status", BlobQuarantined).Error; err != nil {
		t.Fatal(err)
	}
	if _, err := assets.CompleteSnapshotUpload(ctx, projectID, snapshotUploadID,
		owner.user.ID, owner.device.ID, owner.session.ID, nil); !errors.Is(err, ErrAssetUnavailable) {
		t.Fatalf("snapshot complete did not revalidate its manifest: %v", err)
	}
	if err := tx.Model(&model.Blob{}).Where("id = ?", completedAsset.Blob.ID).
		Update("status", BlobReady).Error; err != nil {
		t.Fatal(err)
	}
	objects.stage(stagingObjectKey(projectID, snapshotUploadID), snapshotExpected)
	completedSnapshot, err := assets.CompleteSnapshotUpload(ctx, projectID,
		snapshotUploadID, owner.user.ID, owner.device.ID, owner.session.ID, nil)
	if err != nil {
		t.Fatal(err)
	}
	if completedSnapshot.Snapshot.Seq != 0 ||
		completedSnapshot.Snapshot.SchemaVersion != CollaborationProjectFormatVersion ||
		string(completedSnapshot.Snapshot.AssetIDs) != `["`+assetID.String()+`"]` ||
		completedSnapshot.Blob.Status != BlobReady {
		t.Fatalf("canonical exact-head snapshot was not completed: %#v", completedSnapshot)
	}
	var snapshotAssetRefs int64
	if err := tx.Model(&model.ProjectSnapshotAsset{}).
		Where("snapshot_id = ? AND project_id = ? AND asset_id = ?",
			completedSnapshot.Snapshot.ID, projectID, assetID).
		Count(&snapshotAssetRefs).Error; err != nil || snapshotAssetRefs != 1 {
		t.Fatalf("snapshot asset refs = %d, err %v", snapshotAssetRefs, err)
	}
	if err := tx.Model(&model.Blob{}).Where("id = ?", completedAsset.Blob.ID).
		Update("status", BlobQuarantined).Error; err != nil {
		t.Fatal(err)
	}
	if _, err := store.CompletePublication(ctx, projectID, owner.user.ID); !errors.Is(err, ErrAssetUnavailable) {
		t.Fatalf("publication accepted an unready declared asset: %v", err)
	}
	if err := tx.Model(&model.Blob{}).Where("id = ?", completedAsset.Blob.ID).
		Updates(map[string]any{"status": BlobReady, "kind": "sample"}).Error; err != nil {
		t.Fatal(err)
	}
	if _, err := store.CompletePublication(ctx, projectID, owner.user.ID); !errors.Is(err, ErrAssetUnavailable) {
		t.Fatalf("publication accepted a declared asset/blob kind mismatch: %v", err)
	}
	if err := tx.Model(&model.Blob{}).Where("id = ?", completedAsset.Blob.ID).
		Update("kind", "audio").Error; err != nil {
		t.Fatal(err)
	}

	extraExpected := objectstore.ExpectedObject{
		SHA256: digestBytes([]byte("unreferenced pending upload")), Bytes: 27,
		ContentType: "audio/wav",
	}
	extraUploadID := uuid.New()
	if _, err := assets.PrepareAssetUpload(ctx, PrepareAssetUploadInput{
		ProjectID: projectID, UploadID: extraUploadID, AssetID: uuid.New(),
		ActorUserID: owner.user.ID, DeviceID: owner.device.ID,
		ActorSessionID: owner.session.ID, SHA256: extraExpected.SHA256,
		Bytes: extraExpected.Bytes, Kind: "audio", ContentType: extraExpected.ContentType,
		DisplayName: "extra.wav",
	}); err != nil {
		t.Fatal(err)
	}
	if _, err := store.CompletePublication(ctx, projectID, owner.user.ID); !errors.Is(err, ErrConflict) {
		t.Fatalf("publication ignored an open asset upload: %v", err)
	}
	objects.deleteErr = objectstore.ErrProvider
	if err := assets.AbortUpload(ctx, projectID, extraUploadID, owner.user.ID,
		owner.device.ID, owner.session.ID); err != nil {
		t.Fatal(err)
	}
	var abortedCleanup model.ObjectCleanupJob
	if err := tx.Where("object_key = ?", stagingObjectKey(projectID, extraUploadID)).
		First(&abortedCleanup).Error; err != nil || abortedCleanup.Status != objectCleanupPending {
		t.Fatalf("aborted upload cleanup was not durable: job=%#v err=%v", abortedCleanup, err)
	}
	objects.deleteErr = nil
	if err := tx.Model(&model.ObjectCleanupJob{}).Where("id = ?", abortedCleanup.ID).
		Update("retry_available_at", now).Error; err != nil {
		t.Fatal(err)
	}
	if cleaned, err := assets.DrainObjectCleanupJobs(ctx, 8); err != nil || cleaned != 1 {
		t.Fatalf("aborted staging retry cleaned=%d err=%v", cleaned, err)
	}
	extraReadySHA := digestBytes([]byte("ready but unreferenced sample"))
	extraReadyBlob := model.Blob{
		ID: uuid.New(), SHA256: extraReadySHA, Bytes: 29,
		ContentType: "audio/wav", Kind: "sample", Status: BlobReady,
		ObjectKey: finalObjectKey(extraReadySHA), CreatedAt: now, VerifiedAt: &verifiedAt,
	}
	if err := tx.Create(&extraReadyBlob).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Create(&model.ProjectAsset{
		ProjectID: projectID, AssetID: uuid.New(), BlobID: extraReadyBlob.ID,
		Kind: "sample", DisplayName: "unused.wav", CreatedAt: now,
	}).Error; err != nil {
		t.Fatal(err)
	}
	var project model.CloudProject
	if err := tx.First(&project, "id = ?", projectID).Error; err != nil ||
		project.Status != model.ProjectUploading {
		t.Fatalf("failed publication changed project status: project=%#v err=%v", project, err)
	}

	published, err := store.CompletePublication(ctx, projectID, owner.user.ID)
	if err != nil {
		t.Fatal(err)
	}
	if published.Project.Status != model.ProjectActive ||
		published.Project.SnapshotSeq != published.Project.HeadSeq ||
		published.Project.HeadSeq != 0 {
		t.Fatalf("publication did not atomically activate exact head: %#v", published.Project)
	}
	republished, err := store.CompletePublication(ctx, projectID, owner.user.ID)
	if err != nil || republished.Project.ID != published.Project.ID ||
		republished.Project.Status != model.ProjectActive ||
		republished.Project.SnapshotSeq != published.Project.SnapshotSeq {
		t.Fatalf("idempotent publication retry failed: project=%#v err=%v", republished.Project, err)
	}

	bootstrap, err := store.Bootstrap(ctx, projectID, owner.user.ID, 0, 100)
	if err != nil {
		t.Fatal(err)
	}
	if bootstrap.Project.Status != model.ProjectActive || bootstrap.HeadSeq != 0 ||
		bootstrap.Snapshot == nil || bootstrap.Snapshot.ID != completedSnapshot.Snapshot.ID ||
		bootstrap.Snapshot.Seq != 0 ||
		bootstrap.Snapshot.SchemaVersion != CollaborationProjectFormatVersion ||
		string(bootstrap.Snapshot.AssetIDs) != `["`+assetID.String()+`"]` ||
		len(bootstrap.Operations) != 0 || bootstrap.NextAfterSeq != 0 || bootstrap.HasMore {
		t.Fatalf("bootstrap did not return the publication snapshot/head: %#v", bootstrap)
	}
	if len(objects.promoted) != 2 || objects.verifyCalls != 3 {
		t.Fatalf("unexpected object verification activity: promoted=%d calls=%d",
			len(objects.promoted), objects.verifyCalls)
	}

	staleProject, err := store.CreateProject(ctx, CreateProjectInput{
		OwnerUserID: owner.user.ID, Title: "Stale snapshot",
		FormatVersion: CollaborationProjectFormatVersion,
		EngineVersion: "engine-test", MinimumAppVersion: "1.0.0",
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := assets.PrepareSnapshotUpload(ctx, PrepareSnapshotUploadInput{
		ProjectID: staleProject.Project.ID, UploadID: uuid.New(),
		ActorUserID: owner.user.ID, DeviceID: owner.device.ID,
		ActorSessionID: owner.session.ID, Seq: 1,
		SchemaVersion: CollaborationProjectFormatVersion,
		SHA256:        snapshotExpected.SHA256, Bytes: snapshotExpected.Bytes,
		ContentType: snapshotExpected.ContentType,
	}); !errors.Is(err, ErrConflict) {
		t.Fatalf("snapshot away from the exact head was accepted: %v", err)
	}

	revokedAt := now
	if err := tx.Model(&model.DesktopSession{}).Where("id = ?", owner.session.ID).
		Update("revoked_at", revokedAt).Error; err != nil {
		t.Fatal(err)
	}
	if _, err := assets.PrepareAssetUpload(ctx, PrepareAssetUploadInput{
		ProjectID: projectID, UploadID: uuid.New(), AssetID: uuid.New(),
		ActorUserID: owner.user.ID, DeviceID: owner.device.ID,
		ActorSessionID: owner.session.ID, SHA256: assetExpected.SHA256,
		Bytes: assetExpected.Bytes, Kind: "audio", ContentType: assetExpected.ContentType,
		DisplayName: "revoked.wav",
	}); !errors.Is(err, ErrForbidden) {
		t.Fatalf("revoked desktop actor prepared an upload: %v", err)
	}
}
