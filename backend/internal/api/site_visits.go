package api

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
	"unicode"

	"github.com/google/uuid"
	"vltstudio/backend/internal/model"
)

// Reporting days use Moscow time, independent of the browser/server timezone.
var siteStatsZone = time.FixedZone("MSK", 3*60*60)

func siteDay(now time.Time) time.Time {
	now = now.In(siteStatsZone)
	return time.Date(now.Year(), now.Month(), now.Day(), 0, 0, 0, 0, siteStatsZone)
}

func siteVisitorKey(seed []byte, ip, ua string, now time.Time) string {
	// Domain-separated, daily pseudonym. Neither the IP nor the full UA is stored.
	mac := hmac.New(sha256.New, seed)
	mac.Write([]byte("site-visits\x00" + siteDay(now).Format("2006-01-02") + "\x00" + ip + "\x00" + ua))
	return hex.EncodeToString(mac.Sum(nil))
}

func siteVisitPath(raw string) (string, bool) {
	u, err := url.Parse(raw)
	if err != nil || u.IsAbs() || u.Host != "" || !strings.HasPrefix(u.Path, "/") || strings.HasPrefix(u.Path, "//") || len(u.Path) > 512 || strings.ContainsAny(u.Path, "\\") || strings.ContainsFunc(u.Path, unicode.IsControl) {
		return "", false
	}
	// Query strings and fragments can contain password-reset and invitation tokens.
	return u.Path, true
}

func siteBrowser(ua string) (browser, os string) {
	browser, os = "Другой", "Другая"
	switch {
	case strings.Contains(ua, "Edg/") || strings.Contains(ua, "EdgiOS/") || strings.Contains(ua, "EdgA/"):
		browser = "Edge"
	case strings.Contains(ua, "OPR/"):
		browser = "Opera"
	case strings.Contains(ua, "Firefox/") || strings.Contains(ua, "FxiOS/"):
		browser = "Firefox"
	case strings.Contains(ua, "Chrome/") || strings.Contains(ua, "CriOS/"):
		browser = "Chrome"
	case strings.Contains(ua, "Safari/"):
		browser = "Safari"
	}
	switch {
	case strings.Contains(ua, "Android"):
		os = "Android"
	case strings.Contains(ua, "iPhone") || strings.Contains(ua, "iPad"):
		os = "iOS / iPadOS"
	case strings.Contains(ua, "Windows"):
		os = "Windows"
	case strings.Contains(ua, "Macintosh"):
		os = "macOS"
	case strings.Contains(ua, "Linux"):
		os = "Linux"
	}
	return
}

func (s *Server) recordSiteVisit(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	if !s.originAllowed(r, false) {
		writeError(w, r, 403, "origin_not_allowed", "Request origin could not be verified.", nil)
		return
	}
	ua := r.UserAgent()
	lower := strings.ToLower(ua)
	if r.Header.Get("DNT") == "1" || r.Header.Get("Sec-GPC") == "1" || strings.Contains(lower, "bot") || strings.Contains(lower, "crawler") || strings.Contains(lower, "spider") {
		w.WriteHeader(204)
		return
	}
	now := time.Now()
	if !s.limiter.Allow("site-visit:"+requestIP(r), 120, time.Minute, now) {
		w.Header().Set("Retry-After", "60")
		writeError(w, r, 429, "rate_limited", "Too many visits.", nil)
		return
	}
	var input struct {
		ID       uuid.UUID `json:"id"`
		Path     string    `json:"path"`
		Referrer string    `json:"referrer"`
	}
	if !decodeJSONWithLimit(w, r, &input, 4096) {
		return
	}
	path, valid := siteVisitPath(input.Path)
	if !valid || input.ID == uuid.Nil || len(input.Referrer) > 2048 || len(ua) > 2048 {
		writeError(w, r, 400, "invalid_visit", "Invalid visit.", nil)
		return
	}
	referrer := ""
	if u, err := url.Parse(input.Referrer); err == nil && (u.Scheme == "https" || u.Scheme == "http") && len(u.Hostname()) <= 253 {
		referrer = strings.ToLower(u.Hostname())
	}
	var userID *uuid.UUID
	if user, ok := r.Context().Value(ctxUser).(model.User); ok {
		userID = &user.ID
	}
	browser, os := siteBrowser(ua)
	err := s.DB.WithContext(r.Context()).Exec(`INSERT INTO site_visits
  (id, visited_at, visitor_key, user_id, path, referrer_host, browser, os)
  VALUES (?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (id) DO NOTHING`,
		input.ID, now, siteVisitorKey(s.Config.SigningSeed, requestIP(r), ua, now), userID, path, referrer, browser, os).Error
	if err != nil {
		writeError(w, r, 500, "visit_unavailable", "Visit could not be recorded.", nil)
		return
	}
	w.WriteHeader(204)
}

type siteVisitCounts struct {
	Views    int64 `json:"views"`
	Visitors int64 `json:"visitors"`
	Accounts int64 `json:"accounts"`
}
type siteVisitDay struct {
	Day      string `json:"day"`
	Views    int64  `json:"views"`
	Visitors int64  `json:"visitors"`
	Accounts int64  `json:"accounts"`
}
type siteVisitPage struct {
	Path  string `json:"path"`
	Views int64  `json:"views"`
}
type siteVisitRow struct {
	ID           uuid.UUID  `json:"id"`
	VisitedAt    time.Time  `json:"visited_at"`
	VisitorKey   string     `json:"visitor_key"`
	UserID       *uuid.UUID `json:"user_id"`
	Nickname     string     `json:"nickname"`
	Path         string     `json:"path"`
	ReferrerHost string     `json:"referrer_host"`
	Browser      string     `json:"browser"`
	OS           string     `json:"os"`
}

func (s *Server) adminSiteVisits(w http.ResponseWriter, r *http.Request) {
	days := 7
	if raw := r.URL.Query().Get("days"); raw != "" {
		var err error
		days, err = strconv.Atoi(raw)
		if err != nil || (days != 1 && days != 7 && days != 30) {
			writeError(w, r, 400, "invalid_days", "Choose 1, 7 or 30 days.", nil)
			return
		}
	}
	page := 0
	if raw := r.URL.Query().Get("page"); raw != "" {
		var err error
		page, err = strconv.Atoi(raw)
		if err != nil || page < 0 || page > 10000 {
			writeError(w, r, 400, "invalid_page", "Invalid page.", nil)
			return
		}
	}
	now := time.Now()
	today := siteDay(now)
	start := today.AddDate(0, 0, 1-days)
	db := s.DB.WithContext(r.Context())
	var counts siteVisitCounts
	// A stable daily browser estimate also survives sign-in during the same day.
	err := db.Raw(`SELECT count(*) AS views, count(DISTINCT visitor_key) AS visitors,
  count(DISTINCT user_id) AS accounts FROM site_visits WHERE visited_at >= ? AND visited_at <= ?`, today, now).Scan(&counts).Error
	daily := []siteVisitDay{}
	if err == nil {
		err = db.Raw(`SELECT to_char(visited_at AT TIME ZONE 'Europe/Moscow', 'YYYY-MM-DD') AS day,
  count(*) AS views, count(DISTINCT visitor_key) AS visitors, count(DISTINCT user_id) AS accounts
  FROM site_visits WHERE visited_at >= ? AND visited_at <= ? GROUP BY day ORDER BY day`, start, now).Scan(&daily).Error
	}
	pages := []siteVisitPage{}
	if err == nil {
		err = db.Raw(`SELECT path, count(*) AS views FROM site_visits
  WHERE visited_at >= ? AND visited_at <= ? GROUP BY path ORDER BY views DESC, path LIMIT 20`, start, now).Scan(&pages).Error
	}
	rows := []siteVisitRow{}
	if err == nil {
		err = db.Raw(`SELECT v.id, v.visited_at, v.visitor_key, v.user_id, COALESCE(u.nickname, '') AS nickname,
  v.path, v.referrer_host, v.browser, v.os FROM site_visits v LEFT JOIN users u ON u.id = v.user_id
  WHERE v.visited_at >= ? AND v.visited_at <= ? ORDER BY v.visited_at DESC, v.id DESC LIMIT 51 OFFSET ?`, start, now, page*50).Scan(&rows).Error
	}
	if err != nil {
		writeError(w, r, 500, "site_stats_unavailable", "Не удалось загрузить посещения.", nil)
		return
	}
	hasMore := len(rows) > 50
	if hasMore {
		rows = rows[:50]
	}
	// Include quiet days explicitly rather than drawing a misleading continuous chart.
	filled := make([]siteVisitDay, 0, days)
	byDay := make(map[string]siteVisitDay, len(daily))
	for _, row := range daily {
		byDay[row.Day] = row
	}
	for d := start; !d.After(today); d = d.AddDate(0, 0, 1) {
		key := d.Format("2006-01-02")
		row := byDay[key]
		row.Day = key
		filled = append(filled, row)
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, 200, map[string]any{"today": counts, "daily": filled, "pages": pages, "visits": rows, "has_more": hasMore, "generated_at": now, "timezone": "Europe/Moscow"})
}
