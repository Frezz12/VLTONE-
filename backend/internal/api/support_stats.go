package api

import (
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
)

// Only aggregate explicit link activations; no visitor identifiers or IPs are stored.
func (s *Server) recordSupportClick(w http.ResponseWriter, r *http.Request) {
	if !s.originAllowed(r, false) {
		writeError(w, r, http.StatusForbidden, "origin_not_allowed", "Request origin could not be verified.", nil)
		return
	}
	source := chi.URLParam(r, "source")
	if source != "home" && source != "footer" {
		writeError(w, r, http.StatusBadRequest, "invalid_source", "Unknown support link placement.", nil)
		return
	}
	err := s.DB.WithContext(r.Context()).Exec(`
        INSERT INTO support_link_clicks (source, clicks) VALUES (?, 1)
        ON CONFLICT (source) DO UPDATE
        SET clicks = support_link_clicks.clicks + 1, last_clicked_at = now()`, source).Error
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "support_click_failed", "Support click could not be recorded.", nil)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusNoContent)
}

type supportClickStats struct {
	Source        string     `json:"source"`
	Clicks        int64      `json:"clicks"`
	LastClickedAt *time.Time `json:"last_clicked_at"`
}

func (s *Server) adminSupportStats(w http.ResponseWriter, r *http.Request) {
	rows := []supportClickStats{}
	err := s.DB.WithContext(r.Context()).Raw(`
        SELECT placements.source, COALESCE(c.clicks, 0) AS clicks, c.last_clicked_at
        FROM (VALUES ('home'), ('footer')) AS placements(source)
        LEFT JOIN support_link_clicks c USING (source)
        ORDER BY placements.source DESC`).Scan(&rows).Error
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "support_stats_unavailable", "Support statistics are unavailable.", nil)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, map[string]any{"clicks": rows})
}
