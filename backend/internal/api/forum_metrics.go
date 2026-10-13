package api

import (
	"context"
	"fmt"
	"github.com/go-chi/chi/v5/middleware"
	"net/http"
	"strings"
	"time"
	"vltstudio/backend/internal/model"
)

func (s *Server) observeForum(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		category := ""
		switch {
		case strings.HasPrefix(r.URL.Path, "/v1/forum/auth/") || r.URL.Path == "/v1/desktop/forum-session":
			category = "auth"
		case strings.HasPrefix(r.URL.Path, "/v1/forum/uploads"):
			category = "upload"
		}
		if category == "" || s.metrics == nil {
			next.ServeHTTP(w, r)
			return
		}
		wrapped := middleware.NewWrapResponseWriter(w, r.ProtoMajor)
		next.ServeHTTP(wrapped, r)
		if wrapped.Status() >= 400 {
			if category == "auth" {
				s.metrics.forumAuthFailures.Add(1)
			} else {
				s.metrics.forumUploadFailures.Add(1)
			}
		}
	})
}
func (s *Server) forumMetricsText(r *http.Request) string {
	var out strings.Builder
	if s.metrics != nil {
		fmt.Fprintf(&out, "# TYPE vlt_forum_auth_failures_total counter\nvlt_forum_auth_failures_total %d\n# TYPE vlt_forum_upload_failures_total counter\nvlt_forum_upload_failures_total %d\n", s.metrics.forumAuthFailures.Load(), s.metrics.forumUploadFailures.Load())
	}
	if s.DB == nil {
		return out.String() + "vlt_forum_metrics_database_error 1\n"
	}
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()
	var queue struct {
		Pending int64
		Leased  int64
		Expired int64
		Oldest  float64
	}
	err := s.DB.WithContext(ctx).Raw(`SELECT count(*) FILTER(WHERE lease_until IS NULL) AS pending,count(*) FILTER(WHERE lease_until>=now()) AS leased,count(*) FILTER(WHERE lease_until<now()) AS expired,COALESCE(EXTRACT(EPOCH FROM now()-min(created_at)),0) AS oldest FROM forum_media_jobs`).Scan(&queue).Error
	if err != nil {
		return out.String() + "vlt_forum_metrics_database_error 1\n"
	}
	fmt.Fprintf(&out, "vlt_forum_metrics_database_error 0\nvlt_forum_media_pending %d\nvlt_forum_media_processing %d\nvlt_forum_media_expired_leases %d\nvlt_forum_media_oldest_seconds %.0f\n", queue.Pending, queue.Leased, queue.Expired, queue.Oldest)
	var failed int64
	if s.DB.WithContext(ctx).Model(&model.ForumAttachment{}).Where("media_status='failed'").Count(&failed).Error == nil {
		fmt.Fprintf(&out, "vlt_forum_media_failed %d\n", failed)
	}
	return out.String()
}
