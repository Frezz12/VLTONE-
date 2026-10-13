package api

import (
	"bytes"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/google/uuid"
	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/forumfiles"
	"vltstudio/backend/internal/model"
	"vltstudio/backend/migrations"
)

func TestForumIdentityAndCursor(t *testing.T) {
	verifier := "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
	if pkceChallenge(verifier) != "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM" {
		t.Fatal("RFC 7636 challenge mismatch")
	}
	now := time.Now().UTC()
	id := uuid.New()
	c, e := decodeForumCursor(encodeForumCursor(now, id))
	if e != nil || !c.At.Equal(now) || c.ID != id {
		t.Fatal(c, e)
	}
	for _, raw := range []string{"garbage", strings.Repeat("a", 257), "e30"} {
		if _, e := decodeForumCursor(raw); e == nil {
			t.Fatalf("accepted cursor %q", raw)
		}
	}
	for _, name := range []string{"x.vltmini", "x.vltnode", "x.vltcreator", "x.vlts", "x.vltmorph", "x.vltslicer", "x.vlttheme", "x.vltlang.json", "x.vlt", "x.vlt.zip", "x.vltt.zip"} {
		kind, _, _ := forumFileType(name)
		if kind != "vlt" {
			t.Fatal(name)
		}
	}
	for _, link := range []string{"javascript:alert(1)", "data:text/html,x", "//evil.test", "https://name:password@example.com"} {
		if validForumLink(link) {
			t.Fatal(link)
		}
	}
}

func TestPostgresForumCommunity(t *testing.T) {
	dsn := os.Getenv("VLT_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("set VLT_TEST_DATABASE_URL to an isolated, disposable PostgreSQL database")
	}
	raw, err := sql.Open("pgx", dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	if _, err = raw.Exec("DROP SCHEMA public CASCADE; CREATE SCHEMA public"); err != nil {
		t.Fatal(err)
	}
	if err = migrations.Up(context.Background(), raw); err != nil {
		t.Fatal(err)
	}
	defer migrateDownAll(t, context.Background(), raw)
	db, err := database.Open(dsn, false)
	if err != nil {
		t.Fatal(err)
	}
	conn, _ := db.DB()
	defer conn.Close()
	cfg := config.Config{Environment: "development", PublicOrigin: "http://localhost:3000", ForumOrigin: "http://localhost:3002", AdminOrigin: "http://localhost:3001", DesktopAPIOrigin: "http://localhost:8080", StorageRoot: t.TempDir(), SigningSeed: make([]byte, 32), ForumConfig: config.ForumConfig{ForumFileMaxBytes: 1000000000, ForumImageMaxBytes: 10000000, ForumVideoMaxBytes: 250000000, ForumUserMaxBytes: 10000000000, ForumFFmpeg: "ffmpeg", ForumFFprobe: "ffprobe"}}
	s, err := New(cfg, db)
	if err != nil {
		t.Fatal(err)
	}
	router := s.Router()
	now := time.Now().UTC()
	type actor struct {
		user    model.User
		cookies []*http.Cookie
		headers map[string]string
	}
	password := "correct horse battery staple"
	hash, _ := auth.HashPassword(password)
	makeActor := func(name string) actor {
		u := model.User{ID: uuid.New(), Email: name + "@test.local", EmailKey: strings.ToLower(name) + "@test.local", Nickname: name, NicknameKey: strings.ToLower(name), PasswordHash: hash, Status: "active", Locale: "en", ConsentVersion: "test", ConsentAcceptedAt: now, ConsentIP: "127.0.0.1"}
		if e := db.Create(&u).Error; e != nil {
			t.Fatal(e)
		}
		token, _ := auth.RandomToken(48)
		csrf, _ := auth.RandomToken(24)
		session := model.WebSession{ID: uuid.New(), UserID: u.ID, TokenHash: auth.HashToken(token), CSRFToken: csrf, LastSeenAt: now, ExpiresAt: now.Add(time.Hour)}
		if e := db.Create(&session).Error; e != nil {
			t.Fatal(e)
		}
		return actor{u, []*http.Cookie{{Name: webCookie, Value: token}}, map[string]string{"Origin": cfg.ForumOrigin, "X-CSRF-Token": csrf}}
	}
	alice, bob := makeActor("Alice"), makeActor("Bob")
	request := func(t *testing.T, a actor, method, path string, body any, want int) testResponse {
		t.Helper()
		r := performJSON(router, method, path, body, "203.0.113.102:3456", a.cookies, a.headers)
		if r.Status != want {
			t.Fatalf("%s %s = %d, want %d: %v", method, path, r.Status, want, r.Body)
		}
		return r
	}
	t.Run("SSO PKCE state single use expiry and redirect binding", func(t *testing.T) {
		main := alice
		main.headers = map[string]string{"Origin": cfg.PublicOrigin, "X-CSRF-Token": alice.headers["X-CSRF-Token"]}
		verifier := strings.Repeat("a", 43)
		state := strings.Repeat("b", 43)
		authorize := func() testResponse {
			return request(t, main, "POST", "/v1/forum/auth/authorize", map[string]string{"challenge": pkceChallenge(verifier), "state": state, "redirect_uri": cfg.ForumOrigin + "/auth/callback"}, 200)
		}
		request(t, main, "POST", "/v1/forum/auth/authorize", map[string]string{"challenge": pkceChallenge(verifier), "state": state, "redirect_uri": "https://evil.test/auth/callback"}, 422)
		code := authorize().Body["code"].(string)
		exchange := map[string]string{"code": code, "verifier": strings.Repeat("c", 43), "state": state}
		request(t, alice, "POST", "/v1/forum/auth/exchange", exchange, 401)
		exchange["verifier"] = verifier
		exchange["state"] = strings.Repeat("d", 43)
		request(t, alice, "POST", "/v1/forum/auth/exchange", exchange, 401)
		exchange["state"] = state
		var wg sync.WaitGroup
		statuses := make(chan int, 2)
		for i := 0; i < 2; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				statuses <- performJSON(router, "POST", "/v1/forum/auth/exchange", exchange, "203.0.113.103:1234", nil, alice.headers).Status
			}()
		}
		wg.Wait()
		close(statuses)
		success := 0
		for code := range statuses {
			if code == 200 {
				success++
			} else if code != 401 {
				t.Fatal(code)
			}
		}
		if success != 1 {
			t.Fatal("code was not single use", success)
		}
		code = authorize().Body["code"].(string)
		db.Model(&model.ForumSSOCode{}).Where("token_hash = ?", auth.HashToken(code)).Update("expires_at", now.Add(-time.Minute))
		exchange["code"] = code
		request(t, alice, "POST", "/v1/forum/auth/exchange", exchange, 401)
	})
	t.Run("desktop rotation logout and device revoke", func(t *testing.T) {
		login := func() testResponse {
			return request(t, alice, "POST", "/v1/desktop/auth/login", map[string]any{"email": alice.user.Email, "password": password, "installation_id": "10000000-0000-4000-8000-000000000001", "platform": "macos", "device_name": "Test"}, 200)
		}
		native := login()
		desktop := actor{headers: map[string]string{"Authorization": "Bearer " + native.Body["access_token"].(string)}}
		createWeb := func() actor {
			v := request(t, desktop, "POST", "/v1/desktop/forum-session", map[string]any{}, 200)
			return actor{cookies: []*http.Cookie{{Name: webCookie, Value: v.Body["cookie"].(string)}}}
		}
		embedded := createWeb()
		request(t, embedded, "GET", "/v1/forum/bookmarks", nil, 200)
		rotated := request(t, alice, "POST", "/v1/desktop/auth/refresh", map[string]string{"refresh_token": native.Body["refresh_token"].(string), "request_id": uuid.NewString()}, 200)
		desktop.headers["Authorization"] = "Bearer " + rotated.Body["access_token"].(string)
		request(t, embedded, "GET", "/v1/forum/bookmarks", nil, 200)
		request(t, desktop, "POST", "/v1/desktop/auth/logout", map[string]any{}, 204)
		request(t, embedded, "GET", "/v1/forum/bookmarks", nil, 401)
		native = login()
		desktop.headers["Authorization"] = "Bearer " + native.Body["access_token"].(string)
		embedded = createWeb()
		deviceID := native.Body["device"].(map[string]any)["id"].(string)
		request(t, alice, "DELETE", "/v1/me/devices/"+deviceID, nil, 204)
		request(t, embedded, "GET", "/v1/forum/bookmarks", nil, 401)
		login()
		request(t, embedded, "GET", "/v1/forum/bookmarks", nil, 401)
	})
	request(t, alice, "GET", "/v1/forum/users/Alice", nil, 200)
	request(t, alice, "PUT", "/v1/forum/me/profile", map[string]any{"about": "Producer", "links": []any{}}, 200)
	article := request(t, alice, "POST", "/v1/forum/articles", map[string]any{"kind": "post", "body": "First post"}, 201).Body["article"].(map[string]any)
	id := article["id"].(string)
	t.Run("idempotent concurrent social actions and privacy", func(t *testing.T) {
		for _, path := range []string{"/v1/forum/reactions/article/" + id, "/v1/forum/users/" + alice.user.ID.String() + "/follow", "/v1/forum/bookmarks/article/" + id} {
			var wg sync.WaitGroup
			for i := 0; i < 8; i++ {
				wg.Add(1)
				go func(path string) {
					defer wg.Done()
					r := performJSON(router, "PUT", path, map[string]any{}, "203.0.113.104:2345", bob.cookies, bob.headers)
					if r.Status != 200 && r.Status != 204 {
						t.Errorf("concurrent %s: %v", path, r)
					}
				}(path)
			}
			wg.Wait()
		}
		var a model.ForumArticle
		db.First(&a, "id = ?", id)
		if a.LikeCount != 1 {
			t.Fatal(a.LikeCount)
		}
		var n int64
		db.Model(&model.ForumFollow{}).Where("user_id = ? AND author_id = ?", bob.user.ID, alice.user.ID).Count(&n)
		if n != 1 {
			t.Fatal(n)
		}
		db.Model(&model.ForumNotification{}).Where("user_id = ? AND kind = 'follow'", alice.user.ID).Count(&n)
		if n != 1 {
			t.Fatal(n)
		}
		saved := request(t, bob, "GET", "/v1/forum/bookmarks", nil, 200)
		if len(saved.Body["articles"].([]any)) != 1 {
			t.Fatal(saved)
		}
		own := request(t, alice, "GET", "/v1/forum/bookmarks", nil, 200)
		if len(own.Body["articles"].([]any)) != 0 {
			t.Fatal("bookmarks leaked", own)
		}
		request(t, actor{}, "GET", "/v1/forum/bookmarks", nil, 401)
		for i := 0; i < 2; i++ {
			request(t, bob, "DELETE", "/v1/forum/reactions/article/"+id, nil, 200)
		}
		db.First(&a, "id = ?", id)
		if a.LikeCount != 0 {
			t.Fatal(a.LikeCount)
		}
		for i := 0; i < 2; i++ {
			request(t, bob, "DELETE", "/v1/forum/users/"+alice.user.ID.String()+"/follow", nil, 204)
		}
	})
	t.Run("cursor pagination and nested replies", func(t *testing.T) {
		for i := 0; i < 3; i++ {
			request(t, bob, "POST", "/v1/forum/articles", map[string]any{"kind": "post", "body": fmt.Sprint("post ", i)}, 201)
		}
		first := request(t, alice, "GET", "/v1/forum/feed?limit=2", nil, 200)
		cursor := first.Body["next_cursor"].(string)
		if cursor == "" {
			t.Fatal(first)
		}
		second := request(t, alice, "GET", "/v1/forum/feed?limit=2&cursor="+url.QueryEscape(cursor), nil, 200)
		seen := map[string]bool{}
		for _, page := range []testResponse{first, second} {
			for _, v := range page.Body["articles"].([]any) {
				id := v.(map[string]any)["id"].(string)
				if seen[id] {
					t.Fatal("duplicate", id)
				}
				seen[id] = true
			}
		}
		if len(seen) != 4 {
			t.Fatal(seen)
		}
		c := request(t, bob, "POST", "/v1/forum/articles/"+id+"/comments", map[string]string{"body": "A reply"}, 201).Body["comment"].(map[string]any)["id"].(string)
		request(t, alice, "POST", "/v1/forum/articles/"+id+"/comments", map[string]string{"body": "Reply to Bob", "parent_id": c}, 201)
		notice := request(t, bob, "GET", "/v1/forum/notifications", nil, 200)
		if notice.Body["unread"].(float64) != 1 {
			t.Fatal(notice)
		}
		request(t, bob, "PUT", "/v1/forum/notifications/read", map[string]any{}, 204)
		notice = request(t, bob, "GET", "/v1/forum/notifications", nil, 200)
		if notice.Body["unread"].(float64) != 0 {
			t.Fatal(notice)
		}
	})
	t.Run("multipart ownership corruption publication moderation and worker recovery", func(t *testing.T) {
		payload := []byte("not a video: immediately published, decoder rejects later")
		digest := sha256.Sum256(payload)
		upload := request(t, alice, "POST", "/v1/forum/uploads", map[string]any{"name": "demo.mp4", "bytes": len(payload), "sha256": hex.EncodeToString(digest[:])}, 200)
		aid := upload.Body["attachment"].(map[string]any)["id"].(string)
		path := "/v1/forum/uploads/" + aid
		request(t, bob, "POST", path+"/complete", map[string]any{}, 422)
		part := upload.Body["parts"].([]any)[0].(map[string]any)["request"].(map[string]any)["url"].(string)
		put := func(data []byte, want int) {
			req := httptest.NewRequest("PUT", part, bytes.NewReader(data))
			req.Header.Set("Origin", cfg.ForumOrigin)
			w := httptest.NewRecorder()
			router.ServeHTTP(w, req)
			if w.Code != want {
				t.Fatalf("part %d: %s", w.Code, w.Body.String())
			}
		}
		put(payload[:2], 422)
		bad := bytes.Repeat([]byte("x"), len(payload))
		put(bad, 204)
		request(t, alice, "POST", path+"/complete", map[string]any{}, 422)
		put(payload, 204)
		request(t, alice, "POST", path+"/complete", map[string]any{}, 200)
		request(t, alice, "POST", path+"/complete", map[string]any{}, 200)
		request(t, bob, "POST", "/v1/forum/articles", map[string]any{"kind": "post", "body": "Stolen file", "attachment_ids": []string{aid}}, 422)
		post := request(t, alice, "POST", "/v1/forum/articles", map[string]any{"kind": "post", "body": "Video post survives processing failures", "attachment_ids": []string{aid}}, 201).Body["article"].(map[string]any)["id"].(string)
		request(t, bob, "GET", "/v1/forum/articles/"+post, nil, 200)
		request(t, bob, "GET", "/v1/forum/attachments/"+aid, nil, 200)
		var attachment model.ForumAttachment
		db.First(&attachment, "id = ?", aid)
		if attachment.MediaStatus != "pending" {
			t.Fatal(attachment)
		}
		// Simulate a crashed worker with an expired lease; a new worker must reclaim.
		lease := uuid.New()
		db.Model(&model.ForumMediaJob{}).Where("attachment_id = ?", aid).Updates(map[string]any{"lease_until": now.Add(-time.Minute), "lease_id": lease, "attempts": 1})
		worker := forumfiles.Worker{DB: db, Files: s.ForumFiles, Config: cfg}
		worked, e := worker.Once(context.Background())
		if e != nil || !worked {
			t.Fatal(worked, e)
		}
		db.First(&attachment, "id = ?", aid)
		if attachment.MediaStatus != "rejected" {
			t.Fatal(attachment.MediaStatus)
		}
		request(t, bob, "GET", "/v1/forum/articles/"+post, nil, 200)
		request(t, bob, "GET", "/v1/forum/attachments/"+aid+"/download", nil, 404)
		moderator := model.AdminUser{ID: uuid.New(), UserID: &bob.user.ID, Email: bob.user.Email, EmailKey: bob.user.EmailKey, Nickname: "Moderator", Status: "active", Permissions: []string{"forum.read"}}
		if e := db.Create(&moderator).Error; e != nil {
			t.Fatal(e)
		}
		token, _ := auth.RandomToken(48)
		as := model.AdminSession{ID: uuid.New(), AdminUserID: moderator.ID, TokenHash: auth.HashToken(token), CSRFToken: "admin-csrf", ExpiresAt: now.Add(time.Hour), LastSeenAt: now}
		if e := db.Create(&as).Error; e != nil {
			t.Fatal(e)
		}
		mod := actor{cookies: []*http.Cookie{{Name: adminCookie, Value: token}}, headers: map[string]string{"Origin": cfg.AdminOrigin, "X-CSRF-Token": as.CSRFToken}}
		request(t, mod, "GET", "/v1/admin/forum", nil, 200)
		request(t, mod, "PUT", "/v1/admin/forum/content/article/"+post, map[string]string{"status": "hidden"}, 403)
		db.Model(&moderator).Update("permissions", json.RawMessage(`["forum.read","forum.write"]`))
		request(t, mod, "PUT", "/v1/admin/forum/content/article/"+post, map[string]string{"status": "hidden"}, 204)
		request(t, bob, "GET", "/v1/forum/articles/"+post, nil, 404)
		request(t, bob, "GET", "/v1/forum/attachments/"+aid, nil, 404)
		request(t, alice, "PUT", "/v1/forum/articles/"+post, map[string]string{"kind": "post", "body": "Cannot restore hidden myself", "status": "published"}, 403)
		request(t, mod, "PUT", "/v1/admin/forum/content/article/"+post, map[string]string{"status": "published"}, 204)
		request(t, alice, "DELETE", "/v1/forum/attachments/"+aid, nil, 204)
		worker.Cleanup(context.Background())
		if _, e := s.ForumFiles.Open(context.Background(), attachment.ObjectKey); e == nil {
			t.Fatal("deleted object remains")
		}
	})
	t.Run("S3 direct multipart and completion recovery", func(t *testing.T) {
		if os.Getenv("FORUM_TEST_OBJECT_ENDPOINT") == "" {
			t.Skip("set isolated forum S3 test storage credentials")
		}
		remoteCfg := cfg
		remoteCfg.ForumObjectEndpoint = os.Getenv("FORUM_TEST_OBJECT_ENDPOINT")
		remoteCfg.ForumObjectBucket = os.Getenv("FORUM_TEST_OBJECT_BUCKET")
		remoteCfg.ForumObjectRegion = os.Getenv("FORUM_TEST_OBJECT_REGION")
		remoteCfg.ForumObjectAccessKeyID = os.Getenv("FORUM_TEST_OBJECT_ACCESS_KEY_ID")
		remoteCfg.ForumObjectSecretAccessKey = os.Getenv("FORUM_TEST_OBJECT_SECRET_ACCESS_KEY")
		store, e := forumfiles.New(remoteCfg)
		if e != nil {
			t.Fatal(e)
		}
		local := s.ForumFiles
		s.ForumFiles = store
		defer func() { s.ForumFiles = local }()
		payload := bytes.Repeat([]byte("m"), int(forumPartBytes)+71)
		digest := sha256.Sum256(payload)
		upload := request(t, alice, "POST", "/v1/forum/uploads", map[string]any{"name": "multipart.vltmini", "bytes": len(payload), "sha256": hex.EncodeToString(digest[:])}, 200)
		aid := upload.Body["attachment"].(map[string]any)["id"].(string)
		offset := 0
		for _, rawPart := range upload.Body["parts"].([]any) {
			part := rawPart.(map[string]any)
			size := int(part["bytes"].(float64))
			signed := part["request"].(map[string]any)
			req, e := http.NewRequest("PUT", signed["url"].(string), bytes.NewReader(payload[offset:offset+size]))
			if e != nil {
				t.Fatal(e)
			}
			for key, value := range signed["headers"].(map[string]any) {
				if !strings.EqualFold(key, "Content-Length") {
					req.Header.Set(key, value.(string))
				}
			}
			res, e := http.DefaultClient.Do(req)
			if e != nil {
				t.Fatal(e)
			}
			body, _ := io.ReadAll(res.Body)
			res.Body.Close()
			if res.StatusCode != 200 {
				t.Fatalf("direct PUT: %d %s", res.StatusCode, body)
			}
			offset += size
		}
		var attachment model.ForumAttachment
		db.First(&attachment, "id=?", aid)
		defer store.Delete(context.Background(), attachment.ObjectKey)
		staging := "staging/" + aid
		defer store.Delete(context.Background(), staging)
		// Object storage has committed, but the API process died before the DB commit.
		parts, e := store.Remote.ListMultipartParts(context.Background(), staging, attachment.ProviderUploadID, 1000)
		if e != nil {
			t.Fatal(e)
		}
		if e = store.Remote.CompleteMultipart(context.Background(), staging, attachment.ProviderUploadID, parts); e != nil {
			t.Fatal(e)
		}
		request(t, alice, "POST", "/v1/forum/uploads/"+aid+"/resume", map[string]any{}, 200)
		request(t, alice, "POST", "/v1/forum/uploads/"+aid+"/complete", map[string]any{}, 200)
		request(t, alice, "POST", "/v1/forum/uploads/"+aid+"/complete", map[string]any{}, 200)
		file, e := store.Open(context.Background(), attachment.ObjectKey)
		if e != nil {
			t.Fatal(e)
		}
		defer file.Close()
		actual, e := io.ReadAll(file)
		if e != nil || !bytes.Equal(actual, payload) {
			t.Fatal("promoted bytes differ", e)
		}
	})
	t.Run("Playwright real browser flow", func(t *testing.T) {
		if os.Getenv("VLT_TEST_PLAYWRIGHT") != "1" {
			t.Skip("set VLT_TEST_PLAYWRIGHT=1 with pnpm and Chromium installed")
		}
		apiServer := httptest.NewServer(router)
		defer apiServer.Close()
		s.Config.PublicOrigin = "http://127.0.0.1:3020"
		s.Config.ForumOrigin = "http://localhost:3022"
		s.Config.DesktopAPIOrigin = apiServer.URL
		command := exec.Command("pnpm", "--filter", "@vlt/forum", "test")
		command.Dir = "../../.."
		command.Env = append(os.Environ(), "VLT_FORUM_E2E=1", "VLT_API_ORIGIN="+apiServer.URL)
		output, e := command.CombinedOutput()
		if e != nil {
			t.Fatalf("Playwright: %v\n%s", e, output)
		}
		t.Log(string(output))
	})

}
