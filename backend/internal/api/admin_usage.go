package api

import (
	"net/http"
	"strconv"
	"time"

	"github.com/google/uuid"
	"vltstudio/backend/internal/auth"
)

// Stop at the last report for unfinished sessions: a crash or an offline
// device must not keep adding hours. Durations measure open application time,
// not foreground editing; concurrent application sessions count separately.
const sessionDurationSQL = `GREATEST(0, EXTRACT(EPOCH FROM
	LEAST(COALESCE(s.ended_at, s.last_seen_at), ?::timestamptz) - s.started_at))`

type usageSummary struct {
	TotalSeconds  float64 `json:"total_seconds"`
	TotalSessions int64   `json:"total_sessions"`
	Launches24h   int64   `json:"launches_24h"`
	Users24h      int64   `json:"users_24h"`
}

type usageUser struct {
	UserID        uuid.UUID  `json:"user_id"`
	Nickname      string     `json:"nickname"`
	Sessions      int64      `json:"sessions"`
	TotalSeconds  float64    `json:"total_seconds"`
	LastStartedAt *time.Time `json:"last_started_at"`
	LastSeenAt    *time.Time `json:"last_seen_at"`
}

type recentSession struct {
	ID         uuid.UUID  `json:"id"`
	UserID     uuid.UUID  `json:"user_id"`
	Nickname   string     `json:"nickname"`
	AppVersion string     `json:"app_version"`
	StartedAt  time.Time  `json:"started_at"`
	LastSeenAt time.Time  `json:"last_seen_at"`
	EndedAt    *time.Time `json:"ended_at"`
	Seconds    float64    `json:"seconds"`
}

type dashboardUsage struct {
	Summary        usageSummary    `json:"summary"`
	Users          []usageUser     `json:"users"`
	UserCount      int64           `json:"user_count"`
	Page           int             `json:"page"`
	PageSize       int             `json:"page_size"`
	RecentSessions []recentSession `json:"recent_sessions"`
}

func (s *Server) dashboardUsage(r *http.Request, now time.Time) (dashboardUsage, error) {
	result := dashboardUsage{Users: []usageUser{}, RecentSessions: []recentSession{}, PageSize: 20}
	page, _ := strconv.Atoi(r.URL.Query().Get("usage_page"))
	result.Page = max(0, min(page, 100000))
	err := s.DB.Raw(`SELECT COALESCE(SUM(`+sessionDurationSQL+`), 0) AS total_seconds,
		COUNT(*) AS total_sessions,
		COUNT(*) FILTER (WHERE s.started_at >= ?) AS launches24h,
		COUNT(DISTINCT s.user_id) FILTER (WHERE s.last_seen_at >= ?) AS users24h
		FROM telemetry_sessions s`, now, now.Add(-24*time.Hour), now.Add(-24*time.Hour)).Scan(&result.Summary).Error
	if err != nil {
		return result, err
	}
	search := "%" + auth.NormalizeNickname(r.URL.Query().Get("usage_q")) + "%"
	filter := ` WHERE (u.nickname_key LIKE ? OR u.email_key LIKE ?)`
	if err = s.DB.Raw(`SELECT COUNT(*) FROM users u`+filter, search, search).Scan(&result.UserCount).Error; err != nil {
		return result, err
	}
	err = s.DB.Raw(`SELECT u.id AS user_id, u.nickname, COUNT(s.id) AS sessions,
		COALESCE(SUM(`+sessionDurationSQL+`), 0) AS total_seconds,
		MAX(s.started_at) AS last_started_at, MAX(s.last_seen_at) AS last_seen_at
		FROM users u LEFT JOIN telemetry_sessions s ON s.user_id = u.id`+filter+`
		GROUP BY u.id, u.nickname ORDER BY total_seconds DESC, u.id
		LIMIT ? OFFSET ?`, now, search, search, result.PageSize, result.Page*result.PageSize).Scan(&result.Users).Error
	if err != nil {
		return result, err
	}
	err = s.DB.Raw(`SELECT s.id, s.user_id, u.nickname, s.app_version,
		s.started_at, s.last_seen_at, s.ended_at, `+sessionDurationSQL+` AS seconds
		FROM telemetry_sessions s JOIN users u ON u.id = s.user_id
		ORDER BY s.started_at DESC, s.id DESC LIMIT 20`, now).Scan(&result.RecentSessions).Error
	return result, err
}
