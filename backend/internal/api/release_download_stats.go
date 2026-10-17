package api

import (
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"
)

// Count explicit website clicks separately from file GET/Range requests and updater traffic.
// No visitor identifiers or IP addresses are stored.
func (s *Server) recordReleaseDownloadClick(w http.ResponseWriter, r *http.Request) {
	if !s.originAllowed(r, false) {
		writeError(w, r, http.StatusForbidden, "origin_not_allowed", "Request origin could not be verified.", nil)
		return
	}
	version, kind := strings.TrimSpace(chi.URLParam(r, "version")), chi.URLParam(r, "kind")
	result := s.DB.WithContext(r.Context()).Exec(`
        INSERT INTO release_download_clicks (release_id, kind, clicks)
        SELECT a.release_id, a.kind, 1 FROM release_artifacts a
        JOIN releases r ON r.id = a.release_id
        WHERE r.version = ? AND r.status = 'published' AND a.kind = ?
        ON CONFLICT (release_id, kind) DO UPDATE
        SET clicks = release_download_clicks.clicks + 1, last_clicked_at = now()`, version, kind)
	if result.Error != nil {
		writeError(w, r, 500, "download_click_failed", "Download click could not be recorded.", nil)
		return
	}
	if result.RowsAffected == 0 {
		writeError(w, r, 404, "artifact_not_found", "Artifact was not found.", nil)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

type releaseDownloadStats struct {
	ReleaseID     uuid.UUID  `json:"release_id"`
	Version       string     `json:"version"`
	Kind          string     `json:"kind"`
	Clicks        int64      `json:"clicks"`
	LastClickedAt *time.Time `json:"last_clicked_at"`
}

func (s *Server) adminReleaseDownloadStats(w http.ResponseWriter, r *http.Request) {
	rows := []releaseDownloadStats{}
	// Keep historical counts even when an installer is removed or replaced.
	err := s.DB.WithContext(r.Context()).Raw(`
        SELECT r.id AS release_id, r.version, kinds.kind,
               COALESCE(c.clicks, 0) AS clicks, c.last_clicked_at
        FROM releases r
        JOIN (SELECT release_id, kind FROM release_artifacts
              UNION SELECT release_id, kind FROM release_download_clicks) kinds ON kinds.release_id = r.id
        LEFT JOIN release_download_clicks c ON c.release_id = r.id AND c.kind = kinds.kind
        WHERE r.status = 'published'
        ORDER BY r.version_major DESC, r.version_minor DESC, r.version_patch DESC, r.published_at DESC, kinds.kind
    `).Scan(&rows).Error
	if err != nil {
		writeError(w, r, 500, "download_stats_unavailable", "Download statistics are unavailable.", nil)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, map[string]any{"downloads": rows})
}
