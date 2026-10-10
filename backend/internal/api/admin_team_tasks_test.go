package api

import (
	"context"
	"database/sql"
	"github.com/google/uuid"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"
	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
	"vltstudio/backend/migrations"
)

func TestAdminPermissionBoundary(t *testing.T) {
	admin := model.AdminUser{Permissions: []string{"tasks.read"}}
	s := &Server{}
	for _, test := range []struct {
		method, path string
		status       int
	}{
		{"GET", "/v1/admin/me", 204}, {"GET", "/v1/admin/tasks", 204}, {"GET", "/v1/admin/tasks/members", 204},
		{"GET", "/v1/admin/tasks/1/attachments/2", 204}, {"POST", "/v1/admin/tasks", 403},
		{"PUT", "/v1/admin/team", 403}, {"GET", "/v1/admin/team", 403}, {"GET", "/v1/admin/users", 403},
		{"GET", "/v1/admin/unknown", 403}, {"DELETE", "/v1/admin/releases/1", 403},
	} {
		r := contextWith(httptest.NewRequest(test.method, test.path, nil), ctxAdmin, admin)
		w := httptest.NewRecorder()
		s.adminPermissions(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) })).ServeHTTP(w, r)
		if w.Code != test.status {
			t.Errorf("%s %s: %d", test.method, test.path, w.Code)
		}
	}
}
func TestPostgresAdminTeamTasks(t *testing.T) {
	dsn := os.Getenv("VLT_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("set VLT_TEST_DATABASE_URL for integration tests")
	}
	raw, err := sql.Open("pgx", dsn)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = raw.Close() })
	if _, err = raw.Exec("DROP SCHEMA public CASCADE; CREATE SCHEMA public"); err != nil {
		t.Fatal(err)
	}
	if err = migrations.Up(context.Background(), raw); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { migrateDownAll(t, context.Background(), raw) })
	db, err := database.Open(dsn, false)
	if err != nil {
		t.Fatal(err)
	}
	conn, _ := db.DB()
	defer conn.Close()
	cfg := config.Config{Environment: "development", PublicOrigin: "http://localhost:3000", AdminOrigin: "http://localhost:3001", DesktopAPIOrigin: "http://localhost:8080", StorageRoot: t.TempDir(), SigningSeed: make([]byte, 32)}
	s, err := New(cfg, db)
	if err != nil {
		t.Fatal(err)
	}
	router := s.Router()
	password := "correct horse battery staple"
	hash, err := auth.HashPassword(password)
	if err != nil {
		t.Fatal(err)
	}
	owner := model.AdminUser{ID: uuid.New(), Email: "owner@test.local", EmailKey: "owner@test.local", Nickname: "Owner", PasswordHash: hash, Status: model.UserActive, IsOwner: true, Permissions: []string{}}
	if err = db.Create(&owner).Error; err != nil {
		t.Fatal(err)
	}
	user := model.User{ID: uuid.New(), Email: "member@test.local", EmailKey: "member@test.local", Nickname: "Member", NicknameKey: "member", PasswordHash: hash, Status: model.UserActive, Locale: "en", ConsentVersion: "test", ConsentAcceptedAt: time.Now(), ConsentIP: "127.0.0.1"}
	if err = db.Create(&user).Error; err != nil {
		t.Fatal(err)
	}
	remote := "203.0.113.99:1234"
	request := func(method, path string, body any, cookies []*http.Cookie, headers map[string]string, want int) testResponse {
		t.Helper()
		r := performJSON(router, method, path, body, remote, cookies, headers)
		if r.Status != want {
			t.Fatalf("%s %s: got %d want %d: %v", method, path, r.Status, want, r.Body)
		}
		return r
	}
	login := func(email string, want int) testResponse {
		return request("POST", "/v1/admin/auth/login", map[string]string{"email": email, "password": password}, nil, map[string]string{"Origin": cfg.AdminOrigin}, want)
	}
	login(user.Email, 401)
	own := login(owner.Email, 200)
	headers := map[string]string{"Origin": cfg.AdminOrigin, "X-CSRF-Token": own.Body["csrf_token"].(string)}
	grant := func(permissions []string, enabled bool) {
		request("PUT", "/v1/admin/team", map[string]any{"email": user.Email, "permissions": permissions, "enabled": enabled}, own.Cookies, headers, 200)
	}
	request("PUT", "/v1/admin/team", map[string]any{"email": user.Email, "permissions": []string{"tasks.write"}, "enabled": true}, own.Cookies, headers, 422)
	grant([]string{"tasks.read", "tasks.write"}, true)
	member := login(user.Email, 200)
	mh := map[string]string{"Origin": cfg.AdminOrigin, "X-CSRF-Token": member.Body["csrf_token"].(string)}
	memberID := member.Body["admin"].(map[string]any)["id"].(string)
	for _, path := range []string{"/v1/admin/users", "/v1/admin/dashboard", "/v1/admin/team", "/v1/admin/crashes", "/v1/admin/ai/models", "/v1/admin/audit"} {
		request("GET", path, nil, member.Cookies, nil, 403)
	}
	request("PUT", "/v1/admin/team", map[string]any{"email": user.Email, "permissions": []string{"users.write"}, "enabled": true}, member.Cookies, mh, 403)
	request("POST", "/v1/admin/tasks", map[string]any{}, member.Cookies, nil, 403)
	input := map[string]any{"title": "Идея", "description": "Описание", "status": "idea", "priority": "normal", "assignee_id": memberID, "due_date": "2026-12-01"}
	created := request("POST", "/v1/admin/tasks", input, member.Cookies, mh, 201)
	taskID := created.Body["id"].(string)
	path := "/v1/admin/tasks/" + taskID
	if created.Body["author_id"] != memberID || created.Body["number"].(float64) < 1 {
		t.Fatal(created.Body)
	}
	request("GET", "/v1/admin/tasks/members", nil, member.Cookies, nil, 200)
	request("GET", "/v1/admin/tasks", nil, member.Cookies, nil, 200)
	input["status"] = "in_progress"
	input["version"] = 1
	request("PUT", path, input, member.Cookies, mh, 200)
	request("PUT", path, input, member.Cookies, mh, 409)
	input["version"] = 2
	input["status"] = "invalid"
	request("PUT", path, input, member.Cookies, mh, 422)
	request("POST", path+"/comments", map[string]string{"body": "Начинаем работу"}, member.Cookies, mh, 201)
	file := performMultipart(router, "POST", path+"/attachments", "file", "notes.txt", []byte("private task file"), remote, member.Cookies, mh)
	if file.Status != 201 {
		t.Fatalf("upload: %v", file)
	}
	attachmentID := file.Body["id"].(string)
	download := func(task string, cookies []*http.Cookie, want int) {
		t.Helper()
		r := httptest.NewRequest("GET", task+"/attachments/"+attachmentID, nil)
		for _, c := range cookies {
			r.AddCookie(c)
		}
		w := httptest.NewRecorder()
		router.ServeHTTP(w, r)
		if w.Code != want {
			t.Fatalf("download: %d %s", w.Code, w.Body.String())
		}
		if want == 200 && (w.Body.String() != "private task file" || !strings.HasPrefix(w.Header().Get("Content-Disposition"), "attachment")) {
			t.Fatal("unsafe or corrupted download")
		}
	}
	download(path, member.Cookies, 200)
	download(path, nil, 401)
	download("/v1/admin/tasks/"+uuid.NewString(), member.Cookies, 404)
	detail := request("GET", path, nil, member.Cookies, nil, 200)
	if len(detail.Body["comments"].([]any)) != 1 || len(detail.Body["attachments"].([]any)) != 1 {
		t.Fatal(detail.Body)
	}
	if _, ok := detail.Body["attachments"].([]any)[0].(map[string]any)["data"]; ok {
		t.Fatal("attachment bytes leaked in JSON")
	}
	request("DELETE", path+"/attachments/"+attachmentID, nil, member.Cookies, mh, 204)
	for i := 0; i < 21; i++ {
		result := performMultipart(router, "POST", path+"/attachments", "file", "note.txt", []byte("x"), remote, member.Cookies, mh)
		want := 201
		if i == 20 {
			want = 422
		}
		if result.Status != want {
			t.Fatalf("file count limit at %d: %v", i, result)
		}
	}
	oversized := performMultipart(router, "POST", path+"/attachments", "file", "large.bin", make([]byte, (10<<20)+1), remote, member.Cookies, mh)
	if oversized.Status != 422 {
		t.Fatalf("oversized file: %v", oversized)
	}
	request("POST", path+"/comments", map[string]string{"body": "   "}, member.Cookies, mh, 422)
	grant([]string{"tasks.read"}, true)
	request("GET", "/v1/admin/me", nil, member.Cookies, nil, 401)
	reader := login(user.Email, 200)
	rh := map[string]string{"Origin": cfg.AdminOrigin, "X-CSRF-Token": reader.Body["csrf_token"].(string)}
	request("GET", path, nil, reader.Cookies, nil, 200)
	request("POST", "/v1/admin/tasks", input, reader.Cookies, rh, 403)
	request("POST", path+"/comments", map[string]string{"body": "no"}, reader.Cookies, rh, 403)
	grant([]string{"tasks.read"}, false)
	request("GET", path, nil, reader.Cookies, nil, 401)
	login(user.Email, 403)
	grant([]string{"tasks.read"}, true)
	// Current website credentials are used, never a copied password hash.
	password = "a different correct horse password"
	newHash, _ := auth.HashPassword(password)
	if err = db.Model(&user).Updates(map[string]any{"password_hash": newHash, "nickname": "Renamed"}).Error; err != nil {
		t.Fatal(err)
	}
	// Use a fresh IP to avoid the intentional five-attempt login limit.
	remote = "203.0.113.100:1234"
	fresh := login(user.Email, 200)
	if fresh.Body["admin"].(map[string]any)["nickname"] != "Renamed" {
		t.Fatal("stale website identity")
	}
	reset := model.PasswordResetToken{ID: uuid.New(), UserID: user.ID, TokenHash: auth.HashToken("reset-member-token"), ExpiresAt: time.Now().Add(time.Hour)}
	if err = db.Create(&reset).Error; err != nil {
		t.Fatal(err)
	}
	password = "password changed through the website"
	request("POST", "/v1/web/auth/password-reset/confirm", map[string]string{"token": "reset-member-token", "password": password, "password_confirmation": password}, nil, map[string]string{"Origin": cfg.PublicOrigin}, 204)
	request("GET", path, nil, fresh.Cookies, nil, 401)
	fresh = login(user.Email, 200)
	if err = db.Model(&user).Update("status", model.UserSuspended).Error; err != nil {
		t.Fatal(err)
	}
	request("GET", path, nil, fresh.Cookies, nil, 403)
	request("DELETE", "/v1/admin/users/"+user.ID.String(), map[string]string{"password": "correct horse battery staple"}, own.Cookies, headers, 204)
	var deletedMember model.AdminUser
	if err = db.First(&deletedMember, "id = ?", memberID).Error; err != nil {
		t.Fatal(err)
	}
	if deletedMember.Email != "" || deletedMember.UserID != nil || deletedMember.Status != model.UserSuspended {
		t.Fatalf("deleted identity retained: %+v", deletedMember)
	}
	request("GET", path, nil, own.Cookies, nil, 200)
	var auditCount int64
	db.Model(&model.AdminAuditLog{}).Where("action LIKE 'task.%' OR action = 'admin.access.update'").Count(&auditCount)
	if auditCount < 7 {
		t.Fatalf("missing audit entries: %d", auditCount)
	}
}
