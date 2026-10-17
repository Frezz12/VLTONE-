package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"sync"
	"testing"

	"github.com/go-chi/chi/v5"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/migrations"
)

func supportTestRouter(s *Server) http.Handler {
	r := chi.NewRouter()
	r.Post("/v1/support/boosty/click/{source}", s.recordSupportClick)
	return r
}

func TestSupportClickValidation(t *testing.T) {
	s := &Server{Config: config.Config{Environment: "production", PublicOrigin: "https://example.com"}}
	router := supportTestRouter(s)
	for _, tc := range []struct {
		origin, source string
		status         int
	}{
		{"", "home", 403}, {"https://other.example", "home", 403},
		{s.Config.PublicOrigin, "unknown", 400},
	} {
		r := httptest.NewRequest("POST", "/v1/support/boosty/click/"+tc.source, nil)
		r.Header.Set("Origin", tc.origin)
		w := httptest.NewRecorder()
		router.ServeHTTP(w, r)
		if w.Code != tc.status {
			t.Fatalf("%+v: %d", tc, w.Code)
		}
	}
	if got := adminRoutePermission("GET", "/v1/admin/dashboard/support-stats"); got != "dashboard.read" {
		t.Fatal(got)
	}
}

func TestPostgresSupportClicks(t *testing.T) {
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
	// Use the migration in a connection-local table, leaving other test data intact.
	conn.SetMaxOpenConns(1)
	up, err := migrations.Files.ReadFile("000036_support_link_clicks.up.sql")
	if err != nil {
		t.Fatal(err)
	}
	if err = db.Exec(strings.Replace(string(up), "CREATE TABLE", "CREATE TEMP TABLE", 1)).Error; err != nil {
		t.Fatal(err)
	}
	s := &Server{DB: db, Config: config.Config{Environment: "production", PublicOrigin: "https://example.com"}}
	router := supportTestRouter(s)
	check := func(wantHome, wantFooter int64) {
		t.Helper()
		w := httptest.NewRecorder()
		s.adminSupportStats(w, httptest.NewRequest("GET", "/", nil))
		if w.Code != 200 || w.Header().Get("Cache-Control") != "no-store" {
			t.Fatal(w.Code, w.Body.String())
		}
		var data struct {
			Clicks []supportClickStats `json:"clicks"`
		}
		if err := json.Unmarshal(w.Body.Bytes(), &data); err != nil {
			t.Fatal(err)
		}
		if len(data.Clicks) != 2 {
			t.Fatal(data)
		}
		for _, row := range data.Clicks {
			want := wantHome
			if row.Source == "footer" {
				want = wantFooter
			}
			if row.Clicks != want || (want > 0) != (row.LastClickedAt != nil) {
				t.Fatalf("%+v, want %d", row, want)
			}
		}
	}
	check(0, 0)
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			r := httptest.NewRequest("POST", "/v1/support/boosty/click/home", nil)
			r.Header.Set("Origin", s.Config.PublicOrigin)
			w := httptest.NewRecorder()
			router.ServeHTTP(w, r)
			if w.Code != 204 {
				t.Errorf("click: %d", w.Code)
			}
		}()
	}
	wg.Wait()
	r := httptest.NewRequest("POST", "/v1/support/boosty/click/footer", nil)
	r.Header.Set("Origin", s.Config.PublicOrigin)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, r)
	if w.Code != 204 {
		t.Fatal(w.Code)
	}
	check(20, 1)
	if err = db.Exec("INSERT INTO support_link_clicks(source) VALUES ('invalid')").Error; err == nil {
		t.Fatal("missing source constraint")
	}
	down, err := migrations.Files.ReadFile("000036_support_link_clicks.down.sql")
	if err != nil {
		t.Fatal(err)
	}
	if err = db.Exec(string(down)).Error; err != nil {
		t.Fatal(err)
	}
}
