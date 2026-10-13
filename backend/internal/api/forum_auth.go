package api

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"net/http"
	"regexp"
	"time"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

var pkcePattern = regexp.MustCompile(`^[A-Za-z0-9_-]{43}$`)
var verifierPattern = regexp.MustCompile(`^[A-Za-z0-9._~-]{43,128}$`)

func pkceChallenge(v string) string {
	h := sha256.Sum256([]byte(v))
	return base64.RawURLEncoding.EncodeToString(h[:])
}
func (s *Server) forumConfig(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, 200, map[string]any{"public_origin": s.Config.PublicOrigin, "forum_origin": s.Config.ForumOrigin, "video_max_bytes": s.Config.ForumVideoMaxBytes, "video_max_seconds": 300, "file_max_bytes": s.Config.ForumFileMaxBytes, "image_max_bytes": s.Config.ForumImageMaxBytes, "max_attachments": 10})
}
func (s *Server) forumSSOAuthorize(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("Origin") != s.Config.PublicOrigin {
		writeError(w, r, 403, "origin_failed", "Use the main site to sign in.", nil)
		return
	}
	var in struct {
		Challenge   string `json:"challenge"`
		State       string `json:"state"`
		RedirectURI string `json:"redirect_uri"`
	}
	if !decodeJSON(w, r, &in) {
		return
	}
	if !pkcePattern.MatchString(in.Challenge) || !pkcePattern.MatchString(in.State) || in.RedirectURI != s.Config.ForumOrigin+"/auth/callback" {
		forumFailure(w, r, errForumInput)
		return
	}
	if !s.forumAllow(w, r, "sso", 30, time.Minute) {
		return
	}
	raw, err := auth.RandomToken(32)
	if forumFailure(w, r, err) {
		return
	}
	session := r.Context().Value(ctxWebSession).(model.WebSession)
	row := model.ForumSSOCode{ID: uuid.New(), TokenHash: auth.HashToken(raw), UserID: userFrom(r).ID, SourceSessionID: &session.ID, Challenge: in.Challenge, State: in.State, RedirectURI: in.RedirectURI, ExpiresAt: time.Now().UTC().Add(time.Minute)}
	if !forumFailure(w, r, s.DB.Create(&row).Error) {
		w.Header().Set("Cache-Control", "no-store")
		writeJSON(w, 200, map[string]any{"code": raw, "state": in.State, "redirect_uri": in.RedirectURI})
	}
}
func (s *Server) forumSSOExchange(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("Origin") != s.Config.ForumOrigin {
		writeError(w, r, 403, "origin_failed", "Use the forum to finish sign in.", nil)
		return
	}
	var in struct {
		Code     string `json:"code"`
		Verifier string `json:"verifier"`
		State    string `json:"state"`
	}
	if !decodeJSON(w, r, &in) {
		return
	}
	if len(in.Code) > 128 || !verifierPattern.MatchString(in.Verifier) || !pkcePattern.MatchString(in.State) {
		forumFailure(w, r, errForumInput)
		return
	}
	raw, err := auth.RandomToken(48)
	if forumFailure(w, r, err) {
		return
	}
	csrf, err := auth.RandomToken(24)
	if forumFailure(w, r, err) {
		return
	}
	now := time.Now().UTC()
	var session model.WebSession
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		var code model.ForumSSOCode
		if e := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&code, "token_hash = ? AND consumed_at IS NULL AND expires_at > ?", auth.HashToken(in.Code), now).Error; e != nil {
			return e
		}
		challenge := pkceChallenge(in.Verifier)
		if code.RedirectURI != s.Config.ForumOrigin+"/auth/callback" || code.State != in.State || subtle.ConstantTimeCompare([]byte(challenge), []byte(code.Challenge)) != 1 {
			return errForumInput
		}
		var user model.User
		if e := tx.Clauses(clause.Locking{Strength: "SHARE"}).First(&user, "id = ? AND status = 'active'", code.UserID).Error; e != nil {
			return e
		}
		var source model.WebSession
		if code.SourceSessionID == nil {
			return errForumInput
		}
		if e := tx.Clauses(clause.Locking{Strength: "SHARE"}).First(&source, "id = ? AND revoked_at IS NULL AND expires_at > ? AND last_seen_at > ?", *code.SourceSessionID, now, now.Add(-7*24*time.Hour)).Error; e != nil {
			return e
		}
		if !s.forumSessionDeviceValid(tx, source) {
			return errForumInput
		}
		session = model.WebSession{ID: uuid.New(), UserID: user.ID, TokenHash: auth.HashToken(raw), CSRFToken: csrf, LastSeenAt: now, ExpiresAt: now.Add(30 * 24 * time.Hour)}
		if e := tx.Create(&session).Error; e != nil {
			return e
		}
		return tx.Model(&code).Update("consumed_at", now).Error
	})
	if err != nil {
		writeError(w, r, 401, "sso_expired", "Sign-in link expired. Start again.", nil)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	s.setSessionCookie(w, webCookie, raw, session.ExpiresAt)
	writeJSON(w, 200, map[string]any{"csrf_token": csrf})
}
func (s *Server) forumSessionDeviceValid(db *gorm.DB, session model.WebSession) bool {
	if session.DeviceID == nil {
		return true
	}
	var count int64
	err := db.Model(&model.Device{}).Where("id = ? AND user_id = ? AND revoked_at IS NULL", *session.DeviceID, session.UserID).Count(&count).Error
	return err == nil && count == 1
}

// Only the native authenticated client receives this HttpOnly cookie material.
// It installs it through Qt's cookie store; no desktop credential enters JS.
func (s *Server) desktopForumSession(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	device := r.Context().Value(ctxDevice).(model.Device)
	raw, err := auth.RandomToken(48)
	if forumFailure(w, r, err) {
		return
	}
	csrf, err := auth.RandomToken(24)
	if forumFailure(w, r, err) {
		return
	}
	now := time.Now().UTC()
	session := model.WebSession{ID: uuid.New(), UserID: user.ID, DeviceID: &device.ID, TokenHash: auth.HashToken(raw), CSRFToken: csrf, LastSeenAt: now, ExpiresAt: now.Add(30 * 24 * time.Hour)}
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		var actor model.User
		if e := tx.Clauses(clause.Locking{Strength: "SHARE"}).First(&actor, "id = ? AND status = 'active'", user.ID).Error; e != nil {
			return e
		}
		var locked model.Device
		if e := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&locked, "id = ? AND revoked_at IS NULL", device.ID).Error; e != nil {
			return e
		}
		// Check source session again after taking the device lock, covering logout races.
		claims := r.Context().Value(ctxDesktopClaims).(auth.Claims)
		var source model.DesktopSession
		if e := tx.First(&source, "id = ? AND revoked_at IS NULL AND expires_at > ?", claims.SessionID, now).Error; e != nil {
			return e
		}
		if e := tx.Model(&model.WebSession{}).Where("device_id = ? AND revoked_at IS NULL", device.ID).Update("revoked_at", now).Error; e != nil {
			return e
		}
		return tx.Create(&session).Error
	})
	if !forumFailure(w, r, err) {
		w.Header().Set("Cache-Control", "no-store")
		writeJSON(w, 200, map[string]any{"cookie": raw, "cookie_name": webCookie, "origin": s.Config.ForumOrigin, "expires_at": session.ExpiresAt})
	}
}
