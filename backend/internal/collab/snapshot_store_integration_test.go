package collab

import (
	"context"
	"errors"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
)

func TestPostgresAutosnapshotAndExactHeadSessionEnd(t *testing.T) {
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

	now := time.Date(2026, 8, 30, 14, 0, 0, 0, time.UTC)
	suffix := uuid.NewString()
	owner := model.User{
		ID: uuid.New(), Email: "snapshot-" + suffix + "@example.test",
		EmailKey: "snapshot-" + suffix + "@example.test",
		Nickname: "snapshot-" + suffix, NicknameKey: "snapshot-" + suffix,
		PasswordHash: "unused", Locale: "en", Status: model.UserActive,
		ConsentVersion: "2026-08-23", ConsentAcceptedAt: now,
		ConsentIP: "127.0.0.1", CreatedAt: now, UpdatedAt: now,
	}
	if err := tx.Create(&owner).Error; err != nil {
		t.Fatal(err)
	}
	device := model.Device{
		ID: uuid.New(), UserID: owner.ID, InstallID: uuid.NewString(),
		DisplayName: "snapshot host", Platform: "macos", OSVersion: "test",
		AppVersion: "1.0.0", Hardware: datatypes.JSON([]byte(`{}`)),
		FirstSeenAt: now, LastSeenAt: now,
	}
	if err := tx.Create(&device).Error; err != nil {
		t.Fatal(err)
	}
	desktopSession := model.DesktopSession{
		ID: uuid.New(), UserID: owner.ID, DeviceID: device.ID,
		RefreshTokenHash: uuid.NewString(), ReporterTokenHash: uuid.NewString(),
		ReporterExpiresAt: now.Add(72 * time.Hour), ExpiresAt: now.Add(30 * 24 * time.Hour),
		CreatedAt: now, LastSeenAt: now,
	}
	if err := tx.Create(&desktopSession).Error; err != nil {
		t.Fatal(err)
	}
	project := model.CloudProject{
		ID: uuid.New(), OwnerUserID: owner.ID, Title: "Snapshot lifecycle",
		Status: model.ProjectActive, FormatVersion: CollaborationProjectFormatVersion,
		EngineVersion: "engine-test", MinimumAppVersion: "1.0.0",
		HeadSeq: 500, SnapshotSeq: 0, CreatedAt: now.Add(-10 * time.Minute),
		UpdatedAt: now,
	}
	if err := tx.Create(&project).Error; err != nil {
		t.Fatal(err)
	}
	startedAt := now.Add(-10 * time.Minute)
	session := model.ProjectSession{
		ID: uuid.New(), ProjectID: project.ID, CreatedBy: &owner.ID,
		Mode: model.SessionModeIndependent, Status: model.ProjectSessionActive,
		Version: 1, CreatedAt: startedAt, StartedAt: &startedAt, UpdatedAt: now,
	}
	if err := tx.Create(&session).Error; err != nil {
		t.Fatal(err)
	}
	member := model.ProjectSessionMember{
		ID: uuid.New(), SessionID: session.ID, UserID: owner.ID, DeviceID: device.ID,
		DesktopSessionID: &desktopSession.ID, JoinedAt: startedAt, LastSeenAt: now,
	}
	if err := tx.Create(&member).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).
		Update("host_member_id", member.ID).Error; err != nil {
		t.Fatal(err)
	}

	store := NewStore(tx, 8)
	store.Now = func() time.Time { return now }
	dispatches, err := store.ScheduleSnapshotRequests(context.Background(), 500,
		5*time.Minute, 15*time.Second, 16)
	if err != nil {
		t.Fatal(err)
	}
	if len(dispatches) != 1 || dispatches[0].TargetSeq != 500 ||
		dispatches[0].HostMemberID != member.ID ||
		dispatches[0].Reason != model.SnapshotReasonAutosave {
		t.Fatalf("unexpected autosnapshot dispatch: %#v", dispatches)
	}
	if immediate, err := store.ScheduleSnapshotRequests(context.Background(), 500,
		5*time.Minute, 15*time.Second, 16); err != nil || len(immediate) != 0 {
		t.Fatalf("snapshot retry was not throttled: %#v err=%v", immediate, err)
	}

	now = now.Add(16 * time.Second)
	project.HeadSeq = 501
	project.UpdatedAt = now
	if err := tx.Model(&model.CloudProject{}).Where("id = ?", project.ID).
		Updates(map[string]any{"head_seq": project.HeadSeq, "updated_at": now}).Error; err != nil {
		t.Fatal(err)
	}
	dispatches, err = store.ScheduleSnapshotRequests(context.Background(), 500,
		5*time.Minute, 15*time.Second, 16)
	if err != nil {
		t.Fatal(err)
	}
	if len(dispatches) != 1 || dispatches[0].TargetSeq != project.HeadSeq {
		t.Fatalf("stale autosnapshot target was not superseded: %#v", dispatches)
	}

	end, err := store.BeginEndSession(context.Background(), project.ID, session.ID,
		owner.ID, device.ID, desktopSession.ID, 15*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if end.Finalized || end.FinalSeq != project.HeadSeq || end.Dispatch == nil ||
		end.Dispatch.Reason != model.SnapshotReasonSessionEnd {
		t.Fatalf("session end did not wait for exact snapshot: %#v", end)
	}
	if err := tx.First(&session, "id = ?", session.ID).Error; err != nil ||
		session.Status != model.ProjectSessionEnding || session.EndedAt != nil {
		t.Fatalf("session was closed before snapshot: session=%#v err=%v", session, err)
	}
	if err := store.ArchiveProject(context.Background(), project.ID, owner.ID); !errors.Is(err, ErrConflict) {
		t.Fatalf("archive bypassed mandatory final snapshot: %v", err)
	}

	wrongDevice := uuid.New()
	if _, err := pendingSnapshotRequestTx(tx, project.ID, project.HeadSeq,
		owner.ID, wrongDevice, desktopSession.ID, false); err != ErrForbidden {
		t.Fatalf("non-assigned device obtained snapshot request: %v", err)
	}
	request, err := pendingSnapshotRequestTx(tx, project.ID, project.HeadSeq,
		owner.ID, device.ID, desktopSession.ID, true)
	if err != nil {
		t.Fatal(err)
	}
	verifiedAt := now
	blob := model.Blob{
		ID: uuid.New(), SHA256: fmt.Sprintf("%064x", 900), Bytes: 10,
		ContentType: "application/vnd.vltone.project+json", Kind: "project_snapshot",
		ObjectKey: "blobs/exact-head", Status: BlobReady, CreatedAt: now,
		VerifiedAt: &verifiedAt,
	}
	if err := tx.Create(&blob).Error; err != nil {
		t.Fatal(err)
	}
	snapshot := model.ProjectSnapshot{
		ID: uuid.New(), ProjectID: project.ID, Seq: project.HeadSeq,
		BlobID: blob.ID, SchemaVersion: CollaborationProjectFormatVersion,
		CreatedBy: &owner.ID, CreatedAt: now,
	}
	if err := tx.Create(&snapshot).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Model(&model.CloudProject{}).Where("id = ?", project.ID).
		Update("snapshot_seq", snapshot.Seq).Error; err != nil {
		t.Fatal(err)
	}
	finalization, err := completeSnapshotRequestTx(tx, request, snapshot, now)
	if err != nil {
		t.Fatal(err)
	}
	if finalization == nil || finalization.FinalSeq != project.HeadSeq {
		t.Fatalf("exact-head snapshot did not finalize session: %#v", finalization)
	}
	if err := tx.Clauses(clause.Locking{Strength: "SHARE"}).
		First(&session, "id = ?", session.ID).Error; err != nil ||
		session.Status != model.ProjectSessionEnded || session.EndedAt == nil {
		t.Fatalf("verified exact-head snapshot did not end session: session=%#v err=%v",
			session, err)
	}
	if err := store.ArchiveProject(context.Background(), project.ID, owner.ID); err != nil {
		t.Fatalf("archive remained blocked after exact snapshot: %v", err)
	}
}
