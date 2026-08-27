package api

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"testing"
	"time"

	"github.com/google/uuid"
	_ "github.com/jackc/pgx/v5/stdlib"
	"gorm.io/datatypes"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/quota"
	"vltstudio/backend/migrations"
)

type testResponse struct {
	Status  int
	Body    map[string]any
	Cookies []*http.Cookie
}

func performJSON(handler http.Handler, method, path string, body any, remote string, cookies []*http.Cookie, headers map[string]string) testResponse {
	var encoded bytes.Buffer
	if body != nil {
		_ = json.NewEncoder(&encoded).Encode(body)
	}
	req := httptest.NewRequest(method, path, &encoded)
	req.RemoteAddr = remote
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	for _, cookie := range cookies {
		req.AddCookie(cookie)
	}
	for key, value := range headers {
		req.Header.Set(key, value)
	}
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, req)
	result := testResponse{Status: recorder.Code, Cookies: recorder.Result().Cookies(), Body: map[string]any{}}
	_ = json.Unmarshal(recorder.Body.Bytes(), &result.Body)
	return result
}

func migrateDownAll(t *testing.T, ctx context.Context, raw *sql.DB) {
	t.Helper()
	for {
		var remaining int
		if err := raw.QueryRowContext(ctx, "SELECT count(*) FROM schema_migrations").Scan(&remaining); err != nil {
			t.Fatalf("count migrations: %v", err)
		}
		if remaining == 0 {
			return
		}
		if err := migrations.Down(ctx, raw); err != nil {
			t.Fatalf("migrate down: %v", err)
		}
	}
}

func TestPostgresAccountFlow(t *testing.T) {
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
	if _, err := raw.ExecContext(ctx, "DROP SCHEMA public CASCADE; CREATE SCHEMA public"); err != nil {
		t.Fatalf("reset test schema: %v", err)
	}
	if err := migrations.Up(ctx, raw); err != nil {
		t.Fatalf("migrate up: %v", err)
	}
	migrateDownAll(t, ctx, raw)
	var usersTable *string
	if err := raw.QueryRowContext(ctx, "SELECT to_regclass('public.users')::text").Scan(&usersTable); err != nil {
		t.Fatal(err)
	}
	if usersTable != nil {
		t.Fatal("down migration left users table behind")
	}
	if err := migrations.Up(ctx, raw); err != nil {
		t.Fatalf("second migrate up: %v", err)
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
		AdminOrigin: "http://localhost:3001", DesktopAPIOrigin: "http://localhost:8080",
		StorageRoot: t.TempDir(), ConsentVersion: "2026-08-23", SigningSeed: make([]byte, 32),
		AIGlobalMonthlyLimit: 100_000_000,
	}
	server, err := New(cfg, db)
	if err != nil {
		t.Fatal(err)
	}
	router := server.Router()
	password := "correct horse battery staple"
	registration := map[string]any{
		"email": "Test@Example.COM", "nickname": "ＶＬＴТест", "password": password,
		"password_confirmation": password, "locale": "ru", "consent_accepted": true,
		"consent_version": cfg.ConsentVersion,
	}
	created := performJSON(router, http.MethodPost, "/v1/web/auth/register", registration, "203.0.113.10:1234", nil, nil)
	if created.Status != http.StatusCreated {
		t.Fatalf("register status=%d body=%v", created.Status, created.Body)
	}
	csrf, _ := created.Body["csrf_token"].(string)
	if csrf == "" || len(created.Cookies) == 0 {
		t.Fatal("registration did not create a web session")
	}
	var user model.User
	if err := db.First(&user, "email_key = ?", "test@example.com").Error; err != nil {
		t.Fatal(err)
	}
	if user.Nickname != "ＶＬＴТест" || user.NicknameKey != "vltтест" {
		t.Fatalf("nickname spelling/key were not preserved: %q/%q", user.Nickname, user.NicknameKey)
	}

	duplicate := registration
	duplicate["email"] = "test@example.com"
	duplicate["nickname"] = "Another Tester"
	conflict := performJSON(router, http.MethodPost, "/v1/web/auth/register", duplicate, "203.0.113.11:1234", nil, nil)
	if conflict.Status != http.StatusConflict {
		t.Fatalf("case-insensitive duplicate was accepted: %d %v", conflict.Status, conflict.Body)
	}

	known := performJSON(router, http.MethodPost, "/v1/web/auth/login", map[string]string{"email": "test@example.com", "password": "wrong password"}, "203.0.113.12:1234", nil, nil)
	unknown := performJSON(router, http.MethodPost, "/v1/web/auth/login", map[string]string{"email": "nobody@example.com", "password": "wrong password"}, "203.0.113.13:1234", nil, nil)
	if known.Status != unknown.Status || known.Body["code"] != unknown.Body["code"] || known.Body["message"] != unknown.Body["message"] {
		t.Fatalf("login enumerates users: known=%v unknown=%v", known, unknown)
	}

	deviceLogin := func(installID uuid.UUID, remote string) testResponse {
		return performJSON(router, http.MethodPost, "/v1/desktop/auth/login", map[string]any{
			"email": "TEST@example.com", "password": password, "installation_id": installID,
			"device_name": "Test device", "platform": "macos", "os_version": "15.6",
			"app_version": "0.0.1", "hardware": map[string]any{"cpu_model": "Test CPU", "ram_bytes": 16_000_000_000, "project_path": "/secret/project.vlt"},
		}, remote, nil, nil)
	}
	firstID, secondID, thirdID := uuid.New(), uuid.New(), uuid.New()
	first := deviceLogin(firstID, "203.0.113.20:1234")
	second := deviceLogin(secondID, "203.0.113.21:1234")
	third := deviceLogin(thirdID, "203.0.113.22:1234")
	if first.Status != http.StatusOK || second.Status != http.StatusOK {
		t.Fatalf("first two devices failed: %d/%d", first.Status, second.Status)
	}
	if third.Status != http.StatusConflict || third.Body["code"] != "device_limit_reached" {
		t.Fatalf("third device was not rejected: %d %v", third.Status, third.Body)
	}
	var storedDevice model.Device
	if err := db.First(&storedDevice, "user_id = ? AND install_id = ?", user.ID, firstID.String()).Error; err != nil {
		t.Fatal(err)
	}
	if bytes.Contains(storedDevice.Hardware, []byte("project")) || bytes.Contains(storedDevice.Hardware, []byte("/secret")) {
		t.Fatalf("forbidden hardware field was persisted: %s", storedDevice.Hardware)
	}

	missingCSRF := performJSON(router, http.MethodDelete, "/v1/me/devices/"+storedDevice.ID.String(), nil, "203.0.113.10:1234", created.Cookies, nil)
	if missingCSRF.Status != http.StatusForbidden || missingCSRF.Body["code"] != "csrf_failed" {
		t.Fatalf("state change accepted without CSRF: %d %v", missingCSRF.Status, missingCSRF.Body)
	}
	revoked := performJSON(router, http.MethodDelete, "/v1/me/devices/"+storedDevice.ID.String(), nil, "203.0.113.10:1234", created.Cookies, map[string]string{"Origin": cfg.PublicOrigin, "X-CSRF-Token": csrf})
	if revoked.Status != http.StatusNoContent {
		t.Fatalf("device revoke failed: %d %v", revoked.Status, revoked.Body)
	}
	third = deviceLogin(thirdID, "203.0.113.23:1234")
	if third.Status != http.StatusOK {
		t.Fatalf("revocation did not open a device slot: %d %v", third.Status, third.Body)
	}
	reporterExpires, expiresErr := time.Parse(time.RFC3339,
		fmt.Sprint(third.Body["reporter_expires_at"]))
	serverTime, serverTimeErr := time.Parse(time.RFC3339,
		fmt.Sprint(third.Body["server_time"]))
	if expiresErr != nil || serverTimeErr != nil || reporterExpires.Sub(serverTime) < 71*time.Hour {
		t.Fatalf("reporter token does not cover the offline window: expires=%v server=%v errors=%v/%v",
			reporterExpires, serverTime, expiresErr, serverTimeErr)
	}

	access, _ := third.Body["access_token"].(string)
	reporter, _ := third.Body["reporter_token"].(string)
	sessionID, eventID := uuid.New(), uuid.New()
	batch := map[string]any{"events": []any{map[string]any{
		"event_id": eventID, "session_id": sessionID, "kind": "session_started",
		"occurred_at": time.Now().UTC(), "payload": map[string]any{"app_version": "0.0.1", "build_id": "ci", "hardware": map[string]any{"cpu_model": "Test CPU"}},
	}}}
	telemetryHeaders := map[string]string{"Authorization": "Bearer " + reporter}
	accepted := performJSON(router, http.MethodPost, "/v1/desktop/telemetry/batch", batch, "203.0.113.23:1234", nil, telemetryHeaders)
	duplicateBatch := performJSON(router, http.MethodPost, "/v1/desktop/telemetry/batch", batch, "203.0.113.23:1234", nil, telemetryHeaders)
	if accepted.Status != http.StatusAccepted || accepted.Body["accepted"] != float64(1) || duplicateBatch.Body["duplicates"] != float64(1) {
		t.Fatalf("telemetry idempotency failed: first=%v duplicate=%v", accepted, duplicateBatch)
	}
	validSample := map[string]any{"events": []any{map[string]any{
		"event_id": uuid.New(), "session_id": sessionID, "kind": "sample", "occurred_at": time.Now().UTC(),
		"payload": map[string]any{"process_cpu": 12.5, "system_cpu": 30.0, "dsp_load": 18.0, "dsp_peak": 22.0, "xruns": 0, "resident_bytes": 256_000_000, "sample_rate": 48_000, "buffer_frames": 256, "track_count": 8, "clip_count": 14, "plugin_count": 1, "playback_state": "playing", "recording": false, "foreground": true, "plugins": []any{map[string]any{"name": "Synth", "vendor": "Vendor", "version": "1.0", "format": "VST3", "count": 1}}},
	}}}
	storedSample := performJSON(router, http.MethodPost, "/v1/desktop/telemetry/batch", validSample, "203.0.113.23:1234", nil, telemetryHeaders)
	if storedSample.Status != http.StatusAccepted {
		t.Fatalf("valid telemetry sample was rejected: %d %v", storedSample.Status, storedSample.Body)
	}
	var sampleCount int64
	if err := db.Model(&model.TelemetrySample{}).Where("user_id = ?", user.ID).Count(&sampleCount).Error; err != nil || sampleCount != 1 {
		t.Fatalf("monthly telemetry partition did not store sample: count=%d err=%v", sampleCount, err)
	}
	forbiddenPayload := map[string]any{"events": []any{map[string]any{
		"event_id": uuid.New(), "session_id": sessionID, "kind": "sample", "occurred_at": time.Now().UTC(),
		"payload": map[string]any{"playback_state": "stopped", "project_name": "secret.vlt"},
	}}}
	rejected := performJSON(router, http.MethodPost, "/v1/desktop/telemetry/batch", forbiddenPayload, "203.0.113.23:1234", nil, map[string]string{"Authorization": "Bearer " + access})
	if rejected.Status != http.StatusUnprocessableEntity {
		t.Fatalf("forbidden telemetry field was accepted: %d %v", rejected.Status, rejected.Body)
	}

	// Managed AI is authorized once per provider request. The desktop receives
	// the current direct endpoint/key only after a quota reservation, even when
	// the legacy proxy kill switch is off.
	providerKey, err := server.encryptAISecret("integration-provider-secret")
	if err != nil {
		t.Fatal(err)
	}
	managedModel := model.AIModel{
		ID: uuid.New(), DisplayName: "Integration model", Provider: "openai",
		ModelName: "integration-model", EndpointURL: "https://provider.example/v1",
		APIKeyCiphertext: providerKey, Enabled: true,
	}
	if err := db.Create(&managedModel).Error; err != nil {
		t.Fatal(err)
	}
	aiHeaders := map[string]string{"Authorization": "Bearer " + access}
	leasePath := "/v1/desktop/ai/models/" + managedModel.ID.String() + "/lease"
	lease := performJSON(router, http.MethodPost, leasePath,
		map[string]any{"input_bytes": 120, "max_output_tokens": 80},
		"203.0.113.23:1234", nil, aiHeaders)
	if lease.Status != http.StatusCreated || lease.Body["api_key"] != "integration-provider-secret" ||
		lease.Body["endpoint_url"] != "https://provider.example/v1/chat/completions" ||
		lease.Body["reserved_tokens"] != float64(200) {
		t.Fatalf("direct AI lease failed: %d %v", lease.Status, lease.Body)
	}
	reservationID, _ := lease.Body["reservation_id"].(string)
	settled := performJSON(router, http.MethodPost,
		"/v1/desktop/ai/reservations/"+reservationID+"/settle",
		map[string]any{"actual_tokens": 0, "outcome": "provider_rejected"},
		"203.0.113.23:1234", nil, aiHeaders)
	if settled.Status != http.StatusOK {
		t.Fatalf("direct AI settlement failed: %d %v", settled.Status, settled.Body)
	}
	settledQuota, _ := settled.Body["quota"].(map[string]any)
	if settledQuota["reserved_tokens"] != float64(0) {
		t.Fatalf("direct AI reservation remained held: %v", settled.Body)
	}

	cycle, err := server.Quota.Current(user.ID, time.Now().UTC())
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Model(&model.TokenCycle{}).Where("id = ?", cycle.ID).Update("base_limit", 1).Error; err != nil {
		t.Fatal(err)
	}
	exhausted := performJSON(router, http.MethodPost, leasePath,
		map[string]any{"input_bytes": 1, "max_output_tokens": 1},
		"203.0.113.23:1234", nil, aiHeaders)
	if exhausted.Status != http.StatusPaymentRequired || exhausted.Body["code"] != "ai_quota_exhausted" {
		t.Fatalf("exhausted quota issued a credential: %d %v", exhausted.Status, exhausted.Body)
	}
	if err := db.Model(&model.TokenCycle{}).Where("id = ?", cycle.ID).Update("base_limit", model.BaseDemoLimit).Error; err != nil {
		t.Fatal(err)
	}
	if err := db.Model(&model.AIModel{}).Where("id = ?", managedModel.ID).Update("enabled", false).Error; err != nil {
		t.Fatal(err)
	}
	disabled := performJSON(router, http.MethodPost, leasePath,
		map[string]any{"input_bytes": 1, "max_output_tokens": 1},
		"203.0.113.23:1234", nil, aiHeaders)
	if disabled.Status != http.StatusNotFound || disabled.Body["code"] != "ai_model_not_found" {
		t.Fatalf("disabled model issued a credential: %d %v", disabled.Status, disabled.Body)
	}
	if err := db.Delete(&model.AIModel{}, "id = ?", managedModel.ID).Error; err != nil {
		t.Fatal(err)
	}
	deleted := performJSON(router, http.MethodPost, leasePath,
		map[string]any{"input_bytes": 1, "max_output_tokens": 1},
		"203.0.113.23:1234", nil, aiHeaders)
	if deleted.Status != http.StatusNotFound || deleted.Body["code"] != "ai_model_not_found" {
		t.Fatalf("deleted model issued a credential: %d %v", deleted.Status, deleted.Body)
	}

	loggedOut := performJSON(router, http.MethodPost, "/v1/desktop/auth/logout", nil, "203.0.113.23:1234", nil, map[string]string{"Authorization": "Bearer " + access})
	if loggedOut.Status != http.StatusNoContent {
		t.Fatalf("desktop logout failed: %d %v", loggedOut.Status, loggedOut.Body)
	}
	revokedAccess := performJSON(router, http.MethodGet, "/v1/desktop/me", nil, "203.0.113.23:1234", nil, map[string]string{"Authorization": "Bearer " + access})
	if revokedAccess.Status != http.StatusUnauthorized || revokedAccess.Body["code"] != "desktop_session_revoked" {
		t.Fatalf("desktop access survived logout: %d %v", revokedAccess.Status, revokedAccess.Body)
	}

	knownReset := performJSON(router, http.MethodPost, "/v1/web/auth/password-reset/request", map[string]string{"email": user.Email}, "203.0.113.30:1234", nil, nil)
	unknownReset := performJSON(router, http.MethodPost, "/v1/web/auth/password-reset/request", map[string]string{"email": "missing@example.com"}, "203.0.113.31:1234", nil, nil)
	if knownReset.Status != unknownReset.Status || knownReset.Body["accepted"] != unknownReset.Body["accepted"] {
		t.Fatalf("password reset enumerates users: known=%v unknown=%v", knownReset, unknownReset)
	}

	adminHash, err := auth.HashPassword(password)
	if err != nil {
		t.Fatal(err)
	}
	admin := model.AdminUser{ID: uuid.New(), Email: "owner@example.com", EmailKey: "owner@example.com", Nickname: "Owner", PasswordHash: adminHash, Status: model.UserActive}
	if err := db.Create(&admin).Error; err != nil {
		t.Fatal(err)
	}
	adminLogin := performJSON(router, http.MethodPost, "/v1/admin/auth/login", map[string]string{"email": admin.Email, "password": password}, "203.0.113.40:1234", nil, nil)
	if adminLogin.Status != http.StatusOK || len(adminLogin.Cookies) == 0 {
		t.Fatalf("admin login failed: %d %v", adminLogin.Status, adminLogin.Body)
	}
	adminCSRF, _ := adminLogin.Body["csrf_token"].(string)
	badReset := performJSON(router, http.MethodPost, "/v1/admin/users/"+user.ID.String()+"/tokens/reset", map[string]string{"password": ""}, "203.0.113.40:1234", adminLogin.Cookies,
		map[string]string{"Origin": cfg.AdminOrigin, "X-CSRF-Token": adminCSRF})
	if badReset.Status != http.StatusUnauthorized || badReset.Body["code"] != "reauthentication_failed" {
		t.Fatalf("empty admin reauthentication was accepted: %d %v", badReset.Status, badReset.Body)
	}
	server.Quota.GlobalMonthlyLimit = 100
	reservation, err := server.Quota.Reserve(user.ID, "openai", "test", 60, time.Now().UTC())
	if err != nil {
		t.Fatalf("initial global reservation failed: %v", err)
	}
	if err := server.Quota.Settle(reservation.ID, 60, datatypes.JSON([]byte(`{}`)), time.Now().UTC()); err != nil {
		t.Fatalf("initial global settlement failed: %v", err)
	}
	if _, err := server.Quota.Reset(user.ID, admin.ID, time.Now().UTC()); err != nil {
		t.Fatalf("admin quota reset failed: %v", err)
	}
	if _, err := server.Quota.Reserve(user.ID, "openai", "test", 50, time.Now().UTC()); !errors.Is(err, quota.ErrGlobalExhausted) {
		t.Fatalf("admin reset reopened the global AI budget: %v", err)
	}
	dashboard := performJSON(router, http.MethodGet, "/v1/admin/dashboard", nil, "203.0.113.40:1234", adminLogin.Cookies, nil)
	if dashboard.Status != http.StatusOK || dashboard.Body["activity"] == nil || dashboard.Body["ai_daily"] == nil {
		t.Fatalf("admin dashboard aggregates failed: %d %v", dashboard.Status, dashboard.Body)
	}
}
