package api

import (
	"errors"
	"net/http"
	"strings"
	"time"

	"gorm.io/gorm"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

func bearer(r *http.Request) string {
	header := r.Header.Get("Authorization")
	if !strings.HasPrefix(header, "Bearer ") {
		return ""
	}
	return strings.TrimSpace(strings.TrimPrefix(header, "Bearer "))
}

func (s *Server) webAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		cookie, err := r.Cookie(webCookie)
		if err != nil || cookie.Value == "" {
			writeError(w, r, http.StatusUnauthorized, "authentication_required", "Sign in is required.", nil)
			return
		}
		var session model.WebSession
		now := time.Now().UTC()
		err = s.DB.Where("token_hash = ? AND revoked_at IS NULL AND expires_at > ?", auth.HashToken(cookie.Value), now).First(&session).Error
		if err != nil || now.Sub(session.LastSeenAt) > 7*24*time.Hour {
			writeError(w, r, http.StatusUnauthorized, "session_expired", "Your session has expired.", nil)
			return
		}
		var user model.User
		if err := s.DB.First(&user, "id = ?", session.UserID).Error; err != nil || user.Status != model.UserActive {
			writeError(w, r, http.StatusForbidden, "account_unavailable", "This account is unavailable.", nil)
			return
		}
		if now.Sub(session.LastSeenAt) > 5*time.Minute {
			s.DB.Model(&session).Update("last_seen_at", now)
		}
		r = contextWith(r, ctxUser, user)
		r = contextWith(r, ctxWebSession, session)
		next.ServeHTTP(w, r)
	})
}

func (s *Server) webCSRF(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		session := r.Context().Value(ctxWebSession).(model.WebSession)
		if !s.originAllowed(r, false) || r.Header.Get("X-CSRF-Token") != session.CSRFToken {
			writeError(w, r, http.StatusForbidden, "csrf_failed", "Request origin could not be verified.", nil)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) adminAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		cookie, err := r.Cookie(adminCookie)
		if err != nil || cookie.Value == "" {
			writeError(w, r, http.StatusUnauthorized, "authentication_required", "Administrator sign in is required.", nil)
			return
		}
		var session model.AdminSession
		now := time.Now().UTC()
		err = s.DB.Where("token_hash = ? AND revoked_at IS NULL AND expires_at > ?", auth.HashToken(cookie.Value), now).First(&session).Error
		if err != nil || now.Sub(session.LastSeenAt) > 30*time.Minute {
			writeError(w, r, http.StatusUnauthorized, "session_expired", "Administrator session has expired.", nil)
			return
		}
		var admin model.AdminUser
		if err := s.DB.First(&admin, "id = ?", session.AdminUserID).Error; err != nil || admin.Status != model.UserActive {
			writeError(w, r, http.StatusForbidden, "account_unavailable", "Administrator account is unavailable.", nil)
			return
		}
		s.DB.Model(&session).Update("last_seen_at", now)
		r = contextWith(r, ctxAdmin, admin)
		r = contextWith(r, ctxAdminSession, session)
		next.ServeHTTP(w, r)
	})
}

func (s *Server) adminCSRF(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		session := r.Context().Value(ctxAdminSession).(model.AdminSession)
		if !s.originAllowed(r, true) || r.Header.Get("X-CSRF-Token") != session.CSRFToken {
			writeError(w, r, http.StatusForbidden, "csrf_failed", "Request origin could not be verified.", nil)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) desktopAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		claims, err := s.Signer.Verify(bearer(r), "desktop", time.Now().UTC())
		if err != nil {
			writeError(w, r, http.StatusUnauthorized, "access_token_invalid", "Desktop session has expired.", nil)
			return
		}
		var user model.User
		var device model.Device
		var session model.DesktopSession
		if err := s.DB.First(&user, "id = ?", claims.Subject).Error; err != nil || user.Status != model.UserActive {
			writeError(w, r, http.StatusForbidden, "account_unavailable", "This account is unavailable.", nil)
			return
		}
		if err := s.DB.Where("id = ? AND user_id = ? AND revoked_at IS NULL", claims.DeviceID, user.ID).First(&device).Error; err != nil {
			writeError(w, r, http.StatusForbidden, "device_revoked", "This device has been revoked.", nil)
			return
		}
		if err := s.DB.Where("id = ? AND user_id = ? AND device_id = ? AND revoked_at IS NULL AND expires_at > ?",
			claims.SessionID, user.ID, device.ID, time.Now().UTC()).First(&session).Error; err != nil {
			writeError(w, r, http.StatusUnauthorized, "desktop_session_revoked", "Desktop session has been revoked.", nil)
			return
		}
		r = contextWith(r, ctxUser, user)
		r = contextWith(r, ctxDevice, device)
		r = contextWith(r, ctxDesktopClaims, claims)
		next.ServeHTTP(w, r)
	})
}

func (s *Server) desktopOrReporterAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		token := bearer(r)
		if claims, err := s.Signer.Verify(token, "desktop", time.Now().UTC()); err == nil {
			s.desktopAuth(next).ServeHTTP(w, r)
			_ = claims
			return
		}
		var session model.DesktopSession
		now := time.Now().UTC()
		err := s.DB.Where("reporter_token_hash = ? AND revoked_at IS NULL AND reporter_expires_at > ?", auth.HashToken(token), now).First(&session).Error
		if err != nil {
			writeError(w, r, http.StatusUnauthorized, "reporter_token_invalid", "Reporter session has expired.", nil)
			return
		}
		var user model.User
		var device model.Device
		if err := s.DB.First(&user, "id = ?", session.UserID).Error; err != nil || user.Status != model.UserActive {
			writeError(w, r, http.StatusForbidden, "account_unavailable", "This account is unavailable.", nil)
			return
		}
		if err := s.DB.Where("id = ? AND user_id = ? AND revoked_at IS NULL", session.DeviceID, session.UserID).First(&device).Error; err != nil {
			writeError(w, r, http.StatusForbidden, "device_revoked", "This device has been revoked.", nil)
			return
		}
		r = contextWith(r, ctxUser, user)
		r = contextWith(r, ctxDevice, device)
		next.ServeHTTP(w, r)
	})
}

func findUserByEmail(db *gorm.DB, email string) (model.User, error) {
	var user model.User
	err := db.Where("email_key = ?", auth.NormalizeEmail(email)).First(&user).Error
	return user, err
}

func recordNotFound(err error) bool { return errors.Is(err, gorm.ErrRecordNotFound) }
