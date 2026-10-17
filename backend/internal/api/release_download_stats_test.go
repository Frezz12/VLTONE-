package api

import (
	"context"
	"database/sql"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"
	"vltstudio/backend/internal/config"
	"vltstudio/backend/internal/database"
	"vltstudio/backend/internal/model"
	"vltstudio/backend/migrations"
)

func TestPostgresReleaseDownloadClicks(t *testing.T) {
	dsn := os.Getenv("VLT_TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("set VLT_TEST_DATABASE_URL to an isolated test database")
	}
	raw, err := sql.Open("pgx", dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	if err = migrations.Up(context.Background(), raw); err != nil {
		t.Fatal(err)
	}
	db, err := database.Open(dsn, false)
	if err != nil {
		t.Fatal(err)
	}
	conn, _ := db.DB()
	defer conn.Close()
	s := &Server{DB: db, Config: config.Config{Environment: "production", PublicOrigin: "https://example.com", StorageRoot: t.TempDir()}}
	id := uuid.New()
	version := "1.2.3"
	major, minor, patch := 1, 2, 3
	now := time.Now().UTC()
	item := model.Release{PublishedAt: &now, SummaryRU: "Тест", SummaryEN: "Test", ID: id, Version: &version, VersionMajor: &major, VersionMinor: &minor, VersionPatch: &patch, Status: model.ReleasePublished}
	if err = db.Create(&item).Error; err != nil {
		t.Fatal(err)
	}
	defer db.Delete(&item)
	dir := filepath.Join(s.Config.StorageRoot, "releases", id.String())
	if err = os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "setup.exe")
	if err = os.WriteFile(path, []byte("installer"), 0600); err != nil {
		t.Fatal(err)
	}
	artifact := model.ReleaseArtifact{ID: uuid.New(), ReleaseID: id, Kind: "windows-exe", FileName: "setup.exe", MimeType: "application/octet-stream", Path: path, Bytes: 9, SHA256: "abc"}
	if err = db.Create(&artifact).Error; err != nil {
		t.Fatal(err)
	}
	router := chi.NewRouter()
	router.Post("/v1/releases/{version}/download/{kind}/click", s.recordReleaseDownloadClick)
	router.Get("/v1/releases/{version}/download/{kind}", s.downloadReleaseArtifact)
	clickURL := "/v1/releases/1.2.3/download/windows-exe/click"
	request := func(method, path, origin string) int {
		r := httptest.NewRequest(method, path, nil)
		r.Header.Set("Origin", origin)
		w := httptest.NewRecorder()
		router.ServeHTTP(w, r)
		return w.Code
	}
	for _, origin := range []string{"", "https://other.example"} {
		if got := request("POST", clickURL, origin); got != 403 {
			t.Fatalf("foreign/missing origin: %d", got)
		}
	}
	if got := request("POST", "/v1/releases/1.2.3/download/macos-pkg/click", s.Config.PublicOrigin); got != 404 {
		t.Fatalf("missing artifact: %d", got)
	}
	if err = db.Model(&item).Update("status", "draft").Error; err != nil {
		t.Fatal(err)
	}
	if got := request("POST", clickURL, s.Config.PublicOrigin); got != 404 {
		t.Fatalf("draft counted: %d", got)
	}
	if err = db.Model(&item).Update("status", "published").Error; err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if got := request("POST", clickURL, s.Config.PublicOrigin); got != 204 {
				t.Errorf("click: %d", got)
			}
		}()
	}
	wg.Wait()
	for _, header := range []string{"", "bytes=0-2"} {
		r := httptest.NewRequest("GET", "/v1/releases/1.2.3/download/windows-exe", nil)
		r.Header.Set("Range", header)
		w := httptest.NewRecorder()
		router.ServeHTTP(w, r)
		if w.Code != 200 && w.Code != 206 {
			t.Fatalf("file: %d", w.Code)
		}
	}
	check := func() {
		w := httptest.NewRecorder()
		s.adminReleaseDownloadStats(w, httptest.NewRequest("GET", "/", nil))
		if w.Code != 200 {
			t.Fatal(w.Body.String())
		}
		var body struct {
			Downloads []releaseDownloadStats `json:"downloads"`
		}
		if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
			t.Fatal(err)
		}
		for _, r := range body.Downloads {
			if r.ReleaseID == id && r.Kind == artifact.Kind && r.Clicks == 20 && r.LastClickedAt != nil {
				return
			}
		}
		t.Fatalf("lost or duplicate clicks: %s", w.Body.String())
	}
	check()
	if err = db.Delete(&artifact).Error; err != nil {
		t.Fatal(err)
	}
	check()
	artifact.ID = uuid.New()
	if err = db.Create(&artifact).Error; err != nil {
		t.Fatal(err)
	}
	check()
	if got := adminRoutePermission(http.MethodGet, "/v1/admin/releases/download-stats"); got != "releases.read" {
		t.Fatal(got)
	}
}
