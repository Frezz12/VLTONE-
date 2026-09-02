package api

import (
	"context"
	"database/sql"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"testing"
	"time"

	"github.com/google/uuid"
	_ "github.com/jackc/pgx/v5/stdlib"
	"gorm.io/datatypes"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/collab"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
	"vltstudio/backend/migrations"
)

func TestPostgresAdminManagedCollaborationAccess(t *testing.T) {
	dsn := os.Getenv("VLT_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("set VLT_TEST_DATABASE_URL to run PostgreSQL integration tests")
	}
	ctx := context.Background()
	raw, err := sql.Open("pgx", dsn)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = raw.Close() })
	if _, err := raw.ExecContext(ctx,
		"DROP SCHEMA public CASCADE; CREATE SCHEMA public"); err != nil {
		t.Fatalf("reset test schema: %v", err)
	}
	if err := migrations.Up(ctx, raw); err != nil {
		t.Fatalf("migrate up: %v", err)
	}
	t.Cleanup(func() { migrateDownAll(t, context.Background(), raw) })

	db, err := database.Open(dsn, false)
	if err != nil {
		t.Fatal(err)
	}
	sqlDB, _ := db.DB()
	t.Cleanup(func() { _ = sqlDB.Close() })
	cfg := config.Config{
		Environment: "development", PublicOrigin: "http://localhost:3000",
		AdminOrigin:      "http://localhost:3001",
		DesktopAPIOrigin: "http://localhost:8080", StorageRoot: t.TempDir(),
		ConsentVersion: "2026-08-23", SigningSeed: make([]byte, 32),
		CollabMaxParticipants: 8, CollabMaximumObjectBytes: 8 << 30,
		CollabProjectQuotaBytes: 50 << 30, CollabUserQuotaBytes: 100 << 30,
	}
	server, err := New(cfg, db)
	if err != nil {
		t.Fatal(err)
	}
	// New avoids object-store construction while the test still exercises the
	// globally enabled HTTP surface.
	server.Config.CollaborationEnabled = true
	router := server.Router()
	now := time.Now().UTC()

	user := model.User{
		ID: uuid.New(), Email: "collab@example.com",
		EmailKey: "collab@example.com", Nickname: "Collab User",
		NicknameKey: "collab user", PasswordHash: "unused", Locale: "en",
		Status: model.UserActive, ConsentVersion: cfg.ConsentVersion,
		ConsentAcceptedAt: now, ConsentIP: "203.0.113.10",
	}
	if err := db.Create(&user).Error; err != nil {
		t.Fatal(err)
	}
	if err := db.First(&user, "id = ?", user.ID).Error; err != nil {
		t.Fatal(err)
	}
	if user.CollaborationEnabled {
		t.Fatal("new account did not default to collaboration deny")
	}
	device := model.Device{
		ID: uuid.New(), UserID: user.ID, InstallID: uuid.NewString(),
		DisplayName: "Test device", Platform: "macos", OSVersion: "15.6",
		AppVersion: "1.0.0", Hardware: datatypes.JSON([]byte(`{}`)),
		FirstSeenAt: now, LastSeenAt: now,
	}
	if err := db.Create(&device).Error; err != nil {
		t.Fatal(err)
	}
	desktopSession := model.DesktopSession{
		ID: uuid.New(), UserID: user.ID, DeviceID: device.ID,
		RefreshTokenHash:  auth.HashToken("refresh"),
		ReporterTokenHash: auth.HashToken("reporter"),
		ReporterExpiresAt: now.Add(time.Hour), ExpiresAt: now.Add(time.Hour),
		CreatedAt: now, LastSeenAt: now,
	}
	if err := db.Create(&desktopSession).Error; err != nil {
		t.Fatal(err)
	}
	signed, err := server.signDesktopSessionTokens(user, device, desktopSession,
		now)
	if err != nil {
		t.Fatal(err)
	}
	desktopHeaders := map[string]string{
		"Authorization": "Bearer " + signed.AccessToken,
	}

	capabilities := performJSON(router, http.MethodGet,
		"/v1/desktop/capabilities", nil, "203.0.113.10:1234", nil,
		desktopHeaders)
	collaboration, _ := capabilities.Body["collaboration"].(map[string]any)
	if capabilities.Status != http.StatusOK ||
		collaboration["entitled"] != false {
		t.Fatalf("default deny capabilities = %d %v", capabilities.Status,
			capabilities.Body)
	}
	denied := performJSON(router, http.MethodGet, "/v1/desktop/projects", nil,
		"203.0.113.10:1234", nil, desktopHeaders)
	if denied.Status != http.StatusForbidden ||
		denied.Body["code"] != "collaboration_forbidden" {
		t.Fatalf("default deny route = %d %v", denied.Status, denied.Body)
	}

	server.collabAllowedUsers[user.ID] = true
	overridden := performJSON(router, http.MethodGet,
		"/v1/desktop/capabilities", nil, "203.0.113.10:1234", nil,
		desktopHeaders)
	overrideCollaboration, _ :=
		overridden.Body["collaboration"].(map[string]any)
	if overridden.Status != http.StatusOK ||
		overrideCollaboration["entitled"] != true {
		t.Fatalf("environment allowlist did not override DB deny: %d %v",
			overridden.Status, overridden.Body)
	}
	delete(server.collabAllowedUsers, user.ID)

	admin := model.AdminUser{
		ID: uuid.New(), Email: "admin@example.com", EmailKey: "admin@example.com",
		Nickname: "Admin", PasswordHash: "unused", Status: model.UserActive,
	}
	if err := db.Create(&admin).Error; err != nil {
		t.Fatal(err)
	}
	const adminToken = "admin-cookie-token"
	const csrfToken = "admin-csrf-token"
	adminSession := model.AdminSession{
		ID: uuid.New(), AdminUserID: admin.ID,
		TokenHash: auth.HashToken(adminToken), CSRFToken: csrfToken,
		LastSeenAt: now, ExpiresAt: now.Add(time.Hour), CreatedAt: now,
	}
	if err := db.Create(&adminSession).Error; err != nil {
		t.Fatal(err)
	}
	adminCookies := []*http.Cookie{{Name: adminCookie, Value: adminToken}}
	adminHeaders := map[string]string{
		"Origin": cfg.AdminOrigin, "X-CSRF-Token": csrfToken,
	}
	accessPath := "/v1/admin/users/" + user.ID.String() +
		"/collaboration-access"
	missingCSRF := performJSON(router, http.MethodPut, accessPath,
		map[string]any{"enabled": true}, "203.0.113.20:1234", adminCookies,
		nil)
	if missingCSRF.Status != http.StatusForbidden ||
		missingCSRF.Body["code"] != "csrf_failed" {
		t.Fatalf("collaboration access accepted without CSRF: %d %v",
			missingCSRF.Status, missingCSRF.Body)
	}
	unknownField := performJSON(router, http.MethodPut, accessPath,
		map[string]any{"enabled": true, "extra": true},
		"203.0.113.20:1234", adminCookies, adminHeaders)
	missingEnabled := performJSON(router, http.MethodPut, accessPath,
		map[string]any{}, "203.0.113.20:1234", adminCookies, adminHeaders)
	if unknownField.Status != http.StatusBadRequest ||
		missingEnabled.Status != http.StatusBadRequest {
		t.Fatalf("collaboration access body was not exact: unknown=%d missing=%d",
			unknownField.Status, missingEnabled.Status)
	}

	enabled := performJSON(router, http.MethodPut, accessPath,
		map[string]any{"enabled": true}, "203.0.113.20:1234", adminCookies,
		adminHeaders)
	if enabled.Status != http.StatusOK ||
		enabled.Body["collaboration_enabled"] != true {
		t.Fatalf("enable collaboration access = %d %v", enabled.Status,
			enabled.Body)
	}
	if err := db.First(&user, "id = ?", user.ID).Error; err != nil ||
		!user.CollaborationEnabled {
		t.Fatalf("enable was not stored: enabled=%v err=%v",
			user.CollaborationEnabled, err)
	}
	listed := performJSON(router, http.MethodGet, "/v1/desktop/projects", nil,
		"203.0.113.10:1234", nil, desktopHeaders)
	if projects, ok := listed.Body["projects"].([]any); listed.Status != http.StatusOK || !ok || len(projects) != 0 {
		t.Fatalf("enabled empty project list = %d %v", listed.Status,
			listed.Body)
	}

	server.Config.CollaborationEnabled = false
	disabledCapabilities := performJSON(router, http.MethodGet,
		"/v1/desktop/capabilities", nil, "203.0.113.10:1234", nil,
		desktopHeaders)
	disabledCollaboration, _ :=
		disabledCapabilities.Body["collaboration"].(map[string]any)
	if disabledCapabilities.Status != http.StatusOK ||
		disabledCollaboration["enabled"] != false ||
		disabledCollaboration["entitled"] != true {
		t.Fatalf("global switch hid account entitlement: %d %v",
			disabledCapabilities.Status, disabledCapabilities.Body)
	}
	globallyDenied := performJSON(router, http.MethodGet,
		"/v1/desktop/projects", nil, "203.0.113.10:1234", nil,
		desktopHeaders)
	if globallyDenied.Status != http.StatusServiceUnavailable ||
		globallyDenied.Body["code"] != "collaboration_not_enabled" {
		t.Fatalf("global switch did not block collaboration: %d %v",
			globallyDenied.Status, globallyDenied.Body)
	}
	server.Config.CollaborationEnabled = true

	project := model.CloudProject{
		ID: uuid.New(), OwnerUserID: user.ID, Title: "Access test",
		Status: model.ProjectActive, FormatVersion: 7,
		EngineVersion: "test", MinimumAppVersion: "1.0.0",
		CreatedAt: now, UpdatedAt: now,
	}
	if err := db.Create(&project).Error; err != nil {
		t.Fatal(err)
	}
	startedAt := now
	projectSession := model.ProjectSession{
		ID: uuid.New(), ProjectID: project.ID, CreatedBy: &user.ID,
		Mode: model.SessionModeIndependent, Status: model.ProjectSessionActive,
		CreatedAt: now, StartedAt: &startedAt, UpdatedAt: now,
	}
	if err := db.Create(&projectSession).Error; err != nil {
		t.Fatal(err)
	}
	participant := model.ProjectSessionMember{
		ID: uuid.New(), SessionID: projectSession.ID, UserID: user.ID,
		DeviceID: device.ID, DesktopSessionID: &desktopSession.ID,
		JoinedAt: now, LastSeenAt: now,
	}
	if err := db.Create(&participant).Error; err != nil {
		t.Fatal(err)
	}
	if err := db.Model(&projectSession).
		Update("host_member_id", participant.ID).Error; err != nil {
		t.Fatal(err)
	}
	lease := model.ProjectTrackLease{
		ID: uuid.New(), ProjectID: project.ID, SessionID: projectSession.ID,
		TrackID: uuid.New(), LeaseKind: model.TrackLeaseRecord,
		HolderMemberID: participant.ID, AcquiredAt: now, RenewedAt: now,
		ExpiresAt: now.Add(time.Minute),
	}
	if err := db.Create(&lease).Error; err != nil {
		t.Fatal(err)
	}
	projectOwner := model.User{
		ID: uuid.New(), Email: "owner@example.com", EmailKey: "owner@example.com",
		Nickname: "Project Owner", NicknameKey: "project owner",
		PasswordHash: "unused", Locale: "en", Status: model.UserActive,
		ConsentVersion: cfg.ConsentVersion, ConsentAcceptedAt: now,
		ConsentIP: "203.0.113.11",
	}
	if err := db.Create(&projectOwner).Error; err != nil {
		t.Fatal(err)
	}
	if err := db.Model(&project).Update("owner_user_id", projectOwner.ID).Error; err != nil {
		t.Fatal(err)
	}
	projectMember := model.ProjectMember{
		ProjectID: project.ID, UserID: user.ID, Role: model.ProjectRoleEditor,
		JoinedAt: now, UpdatedAt: now,
	}
	if err := db.Create(&projectMember).Error; err != nil {
		t.Fatal(err)
	}
	const expectedRole = model.ProjectRoleEditor
	assertLateActorDenied := func(expectedCode string) {
		t.Helper()
		lateSubscription, subscribeErr := server.Rooms.Subscribe(project.ID,
			user.ID, device.ID, desktopSession.ID, participant.ID, 2,
			collab.DefaultRoomQueueBytes)
		if subscribeErr != nil {
			t.Fatal(subscribeErr)
		}
		actorState, stateErr := server.collaborationLiveActorState(ctx,
			project.ID, projectSession.ID, participant.ID, user.ID, device.ID,
			desktopSession.ID, now)
		closeInfo, allowed := enforceCollaborationSubscriptionAccess(
			lateSubscription, actorState, expectedRole, stateErr)
		if allowed || closeInfo.Code != expectedCode {
			t.Fatalf("late actor state = allowed:%v close:%#v state:%#v err:%v",
				allowed, closeInfo, actorState, stateErr)
		}
		select {
		case <-lateSubscription.Done():
		default:
			t.Fatal("late websocket subscription remained registered")
		}
	}
	validActor, err := server.collaborationLiveActorState(ctx, project.ID,
		projectSession.ID, participant.ID, user.ID, device.ID,
		desktopSession.ID, now)
	if _, allowed := collaborationLiveActorDecision(validActor, err); err != nil || !allowed || validActor.projectRole != expectedRole {
		t.Fatalf("valid websocket actor state = %#v allowed:%v err:%v",
			validActor, allowed, err)
	}

	if err := db.Model(&model.User{}).Where("id = ?", user.ID).
		Update("collaboration_enabled", false).Error; err != nil {
		t.Fatal(err)
	}
	server.disconnectCollaborationUser(user.ID,
		"collaboration_access_disabled")
	assertLateActorDenied("collaboration_access_disabled")
	if err := db.Model(&model.User{}).Where("id = ?", user.ID).
		Update("collaboration_enabled", true).Error; err != nil {
		t.Fatal(err)
	}

	if err := db.Model(&model.User{}).Where("id = ?", user.ID).
		Update("status", model.UserSuspended).Error; err != nil {
		t.Fatal(err)
	}
	server.disconnectCollaborationUser(user.ID, "account_suspended")
	assertLateActorDenied("account_suspended")
	if err := db.Model(&model.User{}).Where("id = ?", user.ID).
		Update("status", model.UserActive).Error; err != nil {
		t.Fatal(err)
	}

	if err := db.Model(&model.Device{}).Where("id = ?", device.ID).
		Update("revoked_at", now).Error; err != nil {
		t.Fatal(err)
	}
	server.Rooms.DisconnectDevice(device.ID,
		collab.RoomClose{Code: "device_revoked", Reason: "device revoked"})
	assertLateActorDenied("device_revoked")
	if err := db.Model(&model.Device{}).Where("id = ?", device.ID).
		Update("revoked_at", nil).Error; err != nil {
		t.Fatal(err)
	}

	if err := db.Model(&model.DesktopSession{}).
		Where("id = ?", desktopSession.ID).Update("revoked_at", now).Error; err != nil {
		t.Fatal(err)
	}
	server.Rooms.DisconnectDesktopSession(desktopSession.ID, collab.RoomClose{
		Code: "desktop_session_revoked", Reason: "desktop session revoked",
	})
	assertLateActorDenied("desktop_session_revoked")
	if err := db.Model(&model.DesktopSession{}).
		Where("id = ?", desktopSession.ID).Update("revoked_at", nil).Error; err != nil {
		t.Fatal(err)
	}

	if err := db.Model(&model.DesktopSession{}).
		Where("id = ?", desktopSession.ID).
		Update("expires_at", now.Add(-time.Minute)).Error; err != nil {
		t.Fatal(err)
	}
	server.Rooms.DisconnectDesktopSession(desktopSession.ID, collab.RoomClose{
		Code: "desktop_session_revoked", Reason: "desktop session expired",
	})
	assertLateActorDenied("desktop_session_revoked")
	if err := db.Model(&model.DesktopSession{}).
		Where("id = ?", desktopSession.ID).
		Update("expires_at", now.Add(time.Hour)).Error; err != nil {
		t.Fatal(err)
	}

	if err := db.Delete(&model.ProjectMember{}, "project_id = ? AND user_id = ?",
		project.ID, user.ID).Error; err != nil {
		t.Fatal(err)
	}
	server.Rooms.DisconnectProjectUser(project.ID, user.ID, collab.RoomClose{
		Code: "member_removed", Reason: "project member removed",
	})
	assertLateActorDenied("member_removed")
	projectMember.Role = expectedRole
	if err := db.Create(&projectMember).Error; err != nil {
		t.Fatal(err)
	}

	if err := db.Model(&projectMember).Update("role",
		model.ProjectRoleViewer).Error; err != nil {
		t.Fatal(err)
	}
	server.Rooms.DisconnectProjectUser(project.ID, user.ID, collab.RoomClose{
		Code: "role_changed", Reason: "project role changed",
	})
	assertLateActorDenied("role_changed")
	if err := db.Model(&projectMember).Update("role", expectedRole).Error; err != nil {
		t.Fatal(err)
	}

	endingProject := model.CloudProject{
		ID: uuid.New(), OwnerUserID: user.ID, Title: "Ending access test",
		Status: model.ProjectActive, FormatVersion: 7,
		EngineVersion: "test", MinimumAppVersion: "1.0.0",
		CreatedAt: now, UpdatedAt: now,
	}
	if err := db.Create(&endingProject).Error; err != nil {
		t.Fatal(err)
	}
	endingSession := model.ProjectSession{
		ID: uuid.New(), ProjectID: endingProject.ID, CreatedBy: &user.ID,
		Mode: model.SessionModeIndependent, Status: model.ProjectSessionEnding,
		CreatedAt: now, StartedAt: &startedAt, UpdatedAt: now,
	}
	if err := db.Create(&endingSession).Error; err != nil {
		t.Fatal(err)
	}
	endingParticipant := model.ProjectSessionMember{
		ID: uuid.New(), SessionID: endingSession.ID, UserID: user.ID,
		DeviceID: device.ID, DesktopSessionID: &desktopSession.ID,
		JoinedAt: now, LastSeenAt: now,
	}
	if err := db.Create(&endingParticipant).Error; err != nil {
		t.Fatal(err)
	}
	if err := db.Model(&endingSession).
		Update("host_member_id", endingParticipant.ID).Error; err != nil {
		t.Fatal(err)
	}
	endingLease := model.ProjectTrackLease{
		ID: uuid.New(), ProjectID: endingProject.ID, SessionID: endingSession.ID,
		TrackID: uuid.New(), LeaseKind: model.TrackLeaseRecord,
		HolderMemberID: endingParticipant.ID, AcquiredAt: now, RenewedAt: now,
		ExpiresAt: now.Add(time.Minute),
	}
	if err := db.Create(&endingLease).Error; err != nil {
		t.Fatal(err)
	}
	socket, err := server.Rooms.Subscribe(project.ID, user.ID, device.ID,
		desktopSession.ID, participant.ID, 2, collab.DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}

	disabled := performJSON(router, http.MethodPut, accessPath,
		map[string]any{"enabled": false}, "203.0.113.20:1234", adminCookies,
		adminHeaders)
	if disabled.Status != http.StatusOK ||
		disabled.Body["collaboration_enabled"] != false {
		t.Fatalf("disable collaboration access = %d %v", disabled.Status,
			disabled.Body)
	}
	revokedActor, actorErr := server.collaborationLiveActorState(ctx,
		project.ID, projectSession.ID, participant.ID, user.ID, device.ID,
		desktopSession.ID, now)
	revokedClose, revokedAllowed := collaborationLiveActorDecision(revokedActor,
		actorErr)
	if actorErr != nil || revokedAllowed ||
		revokedClose.Code != "collaboration_access_disabled" {
		t.Fatalf("disabled actor state = %#v allowed:%v close:%#v err:%v",
			revokedActor, revokedAllowed, revokedClose, actorErr)
	}
	if err := db.First(&user, "id = ?", user.ID).Error; err != nil ||
		user.CollaborationEnabled {
		t.Fatalf("disable was not stored: enabled=%v err=%v",
			user.CollaborationEnabled, err)
	}
	if err := db.First(&participant, "id = ?", participant.ID).Error; err != nil || participant.LeftAt == nil {
		t.Fatalf("live collaboration membership survived disable: left=%v err=%v",
			participant.LeftAt, err)
	}
	if err := db.First(&endingParticipant, "id = ?", endingParticipant.ID).Error; err != nil || endingParticipant.LeftAt == nil {
		t.Fatalf("ending collaboration membership survived disable: left=%v err=%v",
			endingParticipant.LeftAt, err)
	}
	var leases int64
	if err := db.Model(&model.ProjectTrackLease{}).
		Where("holder_member_id IN ?", []uuid.UUID{participant.ID,
			endingParticipant.ID}).Count(&leases).Error; err != nil || leases != 0 {
		t.Fatalf("collaboration lease survived disable: count=%d err=%v",
			leases, err)
	}
	var storedEndingSession model.ProjectSession
	if err := db.First(&storedEndingSession, "id = ?", endingSession.ID).Error; err != nil || storedEndingSession.Status != model.ProjectSessionEnding ||
		storedEndingSession.HostMemberID != nil {
		t.Fatalf("ending session eviction state = status:%q host:%v err:%v",
			storedEndingSession.Status, storedEndingSession.HostMemberID, err)
	}
	select {
	case <-socket.Done():
	default:
		t.Fatal("collaboration socket was not disconnected after disable commit")
	}
	if socket.CloseInfo().Code != "collaboration_access_disabled" {
		t.Fatalf("collaboration socket close code = %q", socket.CloseInfo().Code)
	}
	var storedDesktopSession model.DesktopSession
	if err := db.First(&storedDesktopSession, "id = ?", desktopSession.ID).Error; err != nil || storedDesktopSession.RevokedAt != nil {
		t.Fatalf("normal desktop login was revoked: revoked=%v err=%v",
			storedDesktopSession.RevokedAt, err)
	}
	desktopMe := performJSON(router, http.MethodGet, "/v1/desktop/me", nil,
		"203.0.113.10:1234", nil, desktopHeaders)
	if desktopMe.Status != http.StatusOK {
		t.Fatalf("normal desktop login stopped after collaboration disable: %d %v",
			desktopMe.Status, desktopMe.Body)
	}

	for _, action := range []string{
		"collaboration_access.enable", "collaboration_access.disable",
	} {
		var count int64
		if err := db.Model(&model.AdminAuditLog{}).
			Where("admin_user_id = ? AND action = ?", admin.ID, action).
			Count(&count).Error; err != nil || count != 1 {
			t.Fatalf("audit action %q count=%d err=%v", action, count, err)
		}
	}

	cancelled, cancel := context.WithCancel(context.Background())
	cancel()
	request := httptest.NewRequest(http.MethodGet,
		"/v1/desktop/capabilities", nil).WithContext(cancelled)
	request = contextWith(request, ctxUser, user)
	response := httptest.NewRecorder()
	server.desktopCapabilities(response, request)
	var unavailable testResponse
	unavailable.Status = response.Code
	unavailable.Body = map[string]any{}
	_ = json.Unmarshal(response.Body.Bytes(), &unavailable.Body)
	if unavailable.Status != http.StatusServiceUnavailable ||
		unavailable.Body["code"] != "collaboration_access_unavailable" {
		t.Fatalf("entitlement DB error became a false deny: %d %v",
			unavailable.Status, unavailable.Body)
	}
}
