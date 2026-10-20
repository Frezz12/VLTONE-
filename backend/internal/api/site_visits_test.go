package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/google/uuid"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
	"vltstudio/backend/migrations"
)

func TestSiteVisitsPrivacyAndValidation(t *testing.T) {
	now := time.Date(2026, 10, 18, 21, 0, 0, 0, time.UTC)
	if got := siteDay(now).Format("2006-01-02"); got != "2026-10-19" {
		t.Fatal(got)
	}
	key := siteVisitorKey([]byte("secret"), "203.0.113.1", "Chrome/1", now)
	for _, other := range []string{
		siteVisitorKey([]byte("secret"), "203.0.113.1", "Chrome/1", now.Add(-time.Second)),
		siteVisitorKey([]byte("other"), "203.0.113.1", "Chrome/1", now),
		siteVisitorKey([]byte("secret"), "203.0.113.2", "Chrome/1", now),
	} {
		if key == other {
			t.Fatal("visitor pseudonym did not rotate")
		}
	}
	if key != siteVisitorKey([]byte("secret"), "203.0.113.1", "Chrome/1", now.Add(time.Hour)) {
		t.Fatal("same day split")
	}
	if path, ok := siteVisitPath("/reset-password?token=secret#private"); !ok || path != "/reset-password" {
		t.Fatal(path, ok)
	}
	for _, raw := range []string{"https://evil.test/", "//evil.test/", "/%00", "/%0a", "/a\\b", "", "/" + strings.Repeat("a", 513)} {
		if _, ok := siteVisitPath(raw); ok {
			t.Fatalf("accepted %q", raw)
		}
	}
	s := &Server{Config: config.Config{Environment: "production", PublicOrigin: "https://site.test"}, limiter: newRateLimiter()}
	for _, tc := range []struct {
		origin, body, dnt string
		code              int
	}{
		{"https://evil.test", `{}`, "", 403},
		{s.Config.PublicOrigin, `{}`, "", 400},
		{s.Config.PublicOrigin, `{"id":"bad","path":"/"}`, "", 400},
		{s.Config.PublicOrigin, `{"id":"10000000-0000-4000-8000-000000000001","path":"//evil.test"}`, "", 400},
		{s.Config.PublicOrigin, `{"user_id":"forged"}`, "", 400},
		{s.Config.PublicOrigin, `{}`, "1", 204},
	} {
		r := httptest.NewRequest("POST", "/v1/site-visits", strings.NewReader(tc.body))
		r.Header.Set("Origin", tc.origin)
		r.Header.Set("DNT", tc.dnt)
		w := httptest.NewRecorder()
		s.recordSiteVisit(w, r)
		if w.Code != tc.code {
			t.Fatalf("%+v: %d %s", tc, w.Code, w.Body.String())
		}
	}
	for _, query := range []string{"?days=0", "?days=31", "?days=x", "?page=-1", "?page=10001"} {
		w := httptest.NewRecorder()
		s.adminSiteVisits(w, httptest.NewRequest("GET", "/"+query, nil))
		if w.Code != 400 {
			t.Fatal(query, w.Code)
		}
	}
	if adminRoutePermission("GET", "/v1/admin/analytics") != "analytics.read" || adminRoutePermission("POST", "/v1/admin/analytics") != "unknown" {
		t.Fatal("incorrect permission")
	}
	for _, header := range []string{"Sec-GPC", "User-Agent"} {
		r := httptest.NewRequest("POST", "/v1/site-visits", strings.NewReader(`{}`))
		r.Header.Set("Origin", s.Config.PublicOrigin)
		if header == "Sec-GPC" {
			r.Header.Set(header, "1")
		} else {
			r.Header.Set(header, "ExampleBot/1.0")
		}
		w := httptest.NewRecorder()
		s.recordSiteVisit(w, r)
		if w.Code != 204 {
			t.Fatal("privacy signal/bot not excluded", w.Code)
		}
	}
	s.limiter = newRateLimiter()
	for i := 0; i < 120; i++ {
		s.limiter.Allow("site-visit:192.0.2.1", 120, time.Minute, time.Now())
	}
	r := httptest.NewRequest("POST", "/v1/site-visits", strings.NewReader(`{}`))
	r.Header.Set("Origin", s.Config.PublicOrigin)
	w := httptest.NewRecorder()
	s.recordSiteVisit(w, r)
	if w.Code != 429 {
		t.Fatal("collection was not rate limited", w.Code)
	}
	for _, allowed := range []bool{false, true} {
		admin := model.AdminUser{}
		if allowed {
			admin.Permissions = []string{"analytics.read"}
		}
		r := contextWith(httptest.NewRequest("GET", "/v1/admin/analytics", nil), ctxAdmin, admin)
		w := httptest.NewRecorder()
		s.adminPermissions(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(204) })).ServeHTTP(w, r)
		if (w.Code == 204) != allowed {
			t.Fatal("statistics access leaked")
		}
	}
}

func TestPostgresSiteVisits(t *testing.T) {
	dsn := os.Getenv("VLT_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("set VLT_TEST_DATABASE_URL to an isolated test database")
	}
	db, err := database.Open(dsn, false)
	if err != nil {
		t.Fatal(err)
	}
	conn, _ := db.DB()
	defer conn.Close()
	conn.SetMaxOpenConns(1)
	// Temporary tables shadow real tables; this test never resets a persistent schema.
	if err := db.Exec(`CREATE TEMP TABLE users (id uuid PRIMARY KEY, nickname text NOT NULL)`).Error; err != nil {
		t.Fatal(err)
	}
	up, err := migrations.Files.ReadFile("000037_site_visits.up.sql")
	if err != nil {
		t.Fatal(err)
	}
	for _, statement := range strings.Split(strings.Replace(string(up), "CREATE TABLE", "CREATE TEMP TABLE", 1), ";") {
		if strings.TrimSpace(statement) != "" {
			if err = db.Exec(statement).Error; err != nil {
				t.Fatal(err)
			}
		}
	}
	s := &Server{DB: db, Config: config.Config{Environment: "production", PublicOrigin: "https://site.test", SigningSeed: []byte("test-secret")}, limiter: newRateLimiter()}
	user := model.User{ID: uuid.New(), Nickname: "Тест"}
	if err = db.Exec(`INSERT INTO users VALUES (?, ?)`, user.ID, user.Nickname).Error; err != nil {
		t.Fatal(err)
	}
	id := uuid.NewString()
	record := func(event string, signed bool) {
		t.Helper()
		r := httptest.NewRequest("POST", "/v1/site-visits", strings.NewReader(`{"id":"`+event+`","path":"/account?token=secret","referrer":"https://search.test/private?secret=yes"}`))
		r.Header.Set("Origin", s.Config.PublicOrigin)
		r.Header.Set("User-Agent", "Mozilla/5.0 Windows Chrome/120")
		if signed {
			r = contextWith(r, ctxUser, user)
		}
		w := httptest.NewRecorder()
		s.recordSiteVisit(w, r)
		if w.Code != 204 {
			t.Fatal(w.Code, w.Body.String())
		}
	}
	record(id, false)
	record(id, false)
	record(uuid.NewString(), true)
	check := func(page int) struct {
		Today   siteVisitCounts `json:"today"`
		Daily   []siteVisitDay  `json:"daily"`
		Visits  []siteVisitRow  `json:"visits"`
		HasMore bool            `json:"has_more"`
	} {
		t.Helper()
		w := httptest.NewRecorder()
		target := "/?days=7"
		if page == 1 {
			target += "&page=1"
		}
		s.adminSiteVisits(w, httptest.NewRequest("GET", target, nil))
		if w.Code != 200 || w.Header().Get("Cache-Control") != "no-store" {
			t.Fatal(w.Code, w.Body.String())
		}
		var data struct {
			Today   siteVisitCounts `json:"today"`
			Daily   []siteVisitDay  `json:"daily"`
			Visits  []siteVisitRow  `json:"visits"`
			HasMore bool            `json:"has_more"`
		}
		if err := json.Unmarshal(w.Body.Bytes(), &data); err != nil {
			t.Fatal(err)
		}
		return data
	}
	data := check(0)
	if data.Today.Views != 2 || data.Today.Visitors != 1 || data.Today.Accounts != 1 || len(data.Daily) != 7 || len(data.Visits) != 2 {
		t.Fatalf("bad counts: %+v", data)
	}
	for _, row := range data.Visits {
		if row.Path != "/account" || row.ReferrerHost != "search.test" || row.Browser != "Chrome" || row.OS != "Windows" {
			t.Fatal(row)
		}
	}
	// Moscow midnight and a zero-filled quiet day.
	yesterday := siteDay(time.Now()).Add(-time.Second)
	if err := db.Exec(`INSERT INTO site_visits (id, visited_at, visitor_key, path, browser, os) VALUES (?, ?, 'yesterday', '/', 'Other', 'Other')`, uuid.New(), yesterday).Error; err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 50; i++ {
		record(uuid.NewString(), false)
	}
	data = check(0)
	second := check(1)
	if data.Today.Views != 52 || len(data.Visits) != 50 || !data.HasMore || len(second.Visits) != 3 || second.HasMore || data.Daily[5].Views != 1 {
		t.Fatalf("pagination/day mismatch: %+v / %+v", data, second)
	}
	seen := map[uuid.UUID]bool{}
	for _, row := range append(data.Visits, second.Visits...) {
		if seen[row.ID] {
			t.Fatal("duplicate across pages")
		}
		seen[row.ID] = true
	}
	if err := db.Exec(`DELETE FROM users WHERE id = ?`, user.ID).Error; err != nil {
		t.Fatal(err)
	}
	if check(0).Today.Accounts != 0 {
		t.Fatal("deleted account retained")
	}
	down, _ := migrations.Files.ReadFile("000037_site_visits.down.sql")
	if err := db.Exec(string(down)).Error; err != nil {
		t.Fatal(err)
	}
}
