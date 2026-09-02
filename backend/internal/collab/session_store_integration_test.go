package collab

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"

	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
)

// This test deliberately never migrates or clears the target database. CI may
// point VLT_COLLAB_TEST_DATABASE_URL at an already-migrated isolated database;
// every fixture and reaper mutation is contained in a rolled-back transaction.
func TestPostgresReapStaleSessionMembers(t *testing.T) {
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

	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	newUser := func(suffix string) model.User {
		return model.User{
			ID: uuid.New(), Email: suffix + "@example.test", EmailKey: suffix + "@example.test",
			Nickname: suffix, NicknameKey: suffix, PasswordHash: "unused", Locale: "en",
			Status: model.UserActive, ConsentVersion: "2026-08-23",
			ConsentAcceptedAt: now, ConsentIP: "127.0.0.1", CreatedAt: now, UpdatedAt: now,
		}
	}
	owner, editor := newUser("reaper-owner-"+uuid.NewString()),
		newUser("reaper-editor-"+uuid.NewString())
	if err := tx.Create(&[]model.User{owner, editor}).Error; err != nil {
		t.Fatal(err)
	}
	newDevice := func(user model.User) model.Device {
		return model.Device{
			ID: uuid.New(), UserID: user.ID, InstallID: uuid.NewString(), DisplayName: user.Nickname,
			Platform: "macos", OSVersion: "test", AppVersion: "1.0.0",
			Hardware: datatypes.JSON([]byte(`{}`)), FirstSeenAt: now, LastSeenAt: now,
		}
	}
	ownerDevice, editorDevice := newDevice(owner), newDevice(editor)
	if err := tx.Create(&[]model.Device{ownerDevice, editorDevice}).Error; err != nil {
		t.Fatal(err)
	}
	newDesktopSession := func(user model.User, device model.Device) model.DesktopSession {
		return model.DesktopSession{
			ID: uuid.New(), UserID: user.ID, DeviceID: device.ID,
			RefreshTokenHash: uuid.NewString(), ReporterTokenHash: uuid.NewString(),
			ReporterExpiresAt: now.Add(72 * time.Hour), ExpiresAt: now.Add(30 * 24 * time.Hour),
			CreatedAt: now, LastSeenAt: now,
		}
	}
	ownerAuth, editorAuth := newDesktopSession(owner, ownerDevice),
		newDesktopSession(editor, editorDevice)
	if err := tx.Create(&[]model.DesktopSession{ownerAuth, editorAuth}).Error; err != nil {
		t.Fatal(err)
	}
	project := model.CloudProject{
		ID: uuid.New(), OwnerUserID: owner.ID, Title: "Reaper test",
		Status: model.ProjectActive, FormatVersion: CollaborationProjectFormatVersion,
		EngineVersion: "engine-test", MinimumAppVersion: "1.0.0",
		CreatedAt: now, UpdatedAt: now,
	}
	if err := tx.Create(&project).Error; err != nil {
		t.Fatal(err)
	}
	inviter := owner.ID
	if err := tx.Create(&model.ProjectMember{
		ProjectID: project.ID, UserID: editor.ID, Role: model.ProjectRoleEditor,
		ColorIndex: 1, InvitedBy: &inviter, JoinedAt: now, UpdatedAt: now,
	}).Error; err != nil {
		t.Fatal(err)
	}
	started := now.Add(-time.Minute)
	session := model.ProjectSession{
		ID: uuid.New(), ProjectID: project.ID, CreatedBy: &inviter,
		Mode: model.SessionModeIndependent, Status: model.ProjectSessionActive,
		Version: 1, CreatedAt: started, StartedAt: &started, UpdatedAt: now,
	}
	if err := tx.Create(&session).Error; err != nil {
		t.Fatal(err)
	}
	ownerMember := model.ProjectSessionMember{
		ID: uuid.New(), SessionID: session.ID, UserID: owner.ID, DeviceID: ownerDevice.ID,
		DesktopSessionID: &ownerAuth.ID, JoinedAt: started,
		LastSeenAt: now.Add(-time.Minute),
	}
	editorMember := model.ProjectSessionMember{
		ID: uuid.New(), SessionID: session.ID, UserID: editor.ID, DeviceID: editorDevice.ID,
		DesktopSessionID: &editorAuth.ID, JoinedAt: started.Add(time.Second), LastSeenAt: now,
	}
	if err := tx.Create(&[]model.ProjectSessionMember{ownerMember, editorMember}).Error; err != nil {
		t.Fatal(err)
	}
	if err := tx.Model(&model.ProjectSession{}).Where("id = ?", session.ID).
		Update("host_member_id", ownerMember.ID).Error; err != nil {
		t.Fatal(err)
	}
	lease := model.ProjectTrackLease{
		ID: uuid.New(), ProjectID: project.ID, SessionID: session.ID, TrackID: uuid.New(),
		LeaseKind: model.TrackLeaseRecord, HolderMemberID: ownerMember.ID,
		AcquiredAt: now.Add(-20 * time.Second), RenewedAt: now.Add(-10 * time.Second),
		ExpiresAt: now.Add(10 * time.Second),
	}
	if err := tx.Create(&lease).Error; err != nil {
		t.Fatal(err)
	}
	store := NewStore(tx, 8)
	store.Now = func() time.Time { return now }
	events, err := store.ReapStaleSessionMembers(context.Background(),
		45*time.Second, 256)
	if err != nil {
		t.Fatal(err)
	}
	if len(events) != 1 || len(events[0].MemberIDs) != 1 ||
		events[0].MemberIDs[0] != ownerMember.ID ||
		events[0].PreviousHostMemberID == nil || *events[0].PreviousHostMemberID != ownerMember.ID ||
		events[0].HostMemberID == nil || *events[0].HostMemberID != editorMember.ID {
		t.Fatalf("unexpected reaper event: %#v", events)
	}
	if err := tx.First(&ownerMember, "id = ?", ownerMember.ID).Error; err != nil ||
		ownerMember.LeftAt == nil {
		t.Fatalf("stale member remained active: member=%#v err=%v", ownerMember, err)
	}
	if err := tx.First(&editorMember, "id = ?", editorMember.ID).Error; err != nil ||
		editorMember.LeftAt != nil {
		t.Fatalf("fresh member was evicted: member=%#v err=%v", editorMember, err)
	}
	if err := tx.First(&session, "id = ?", session.ID).Error; err != nil ||
		session.HostMemberID == nil || *session.HostMemberID != editorMember.ID {
		t.Fatalf("host fallback was not committed: session=%#v err=%v", session, err)
	}
	var leaseCount int64
	if err := tx.Model(&model.ProjectTrackLease{}).Where("id = ?", lease.ID).
		Count(&leaseCount).Error; err != nil || leaseCount != 0 {
		t.Fatalf("stale member lease survived: count=%d err=%v", leaseCount, err)
	}
	second, err := store.ReapStaleSessionMembers(context.Background(),
		45*time.Second, 256)
	if err != nil || len(second) != 0 {
		t.Fatalf("reaper was not idempotent: events=%#v err=%v", second, err)
	}
}
