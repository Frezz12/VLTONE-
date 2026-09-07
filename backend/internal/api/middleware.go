package api

import (
	"errors"
	"github.com/google/uuid"
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
		// One statement observes account, device and session revocation in the
		// same database snapshot. Keep the distinct response codes below.
		var access struct {
			User         model.User   `gorm:"embedded"`
			Device       model.Device `gorm:"embedded;embeddedPrefix:device_"`
			SessionValid bool
		}
		result := s.DB.WithContext(r.Context()).Raw(`SELECT u.*,
            d.id AS device_id, d.user_id AS device_user_id, d.install_id AS device_install_id,
            d.display_name AS device_display_name, d.platform AS device_platform,
            d.os_version AS device_os_version, d.app_version AS device_app_version,
            d.hardware AS device_hardware, d.first_seen_at AS device_first_seen_at,
            d.last_seen_at AS device_last_seen_at, d.revoked_at AS device_revoked_at,
            (ds.id IS NOT NULL) AS session_valid
            FROM users u
            LEFT JOIN devices d ON d.id = ? AND d.user_id = u.id AND d.revoked_at IS NULL
            LEFT JOIN desktop_sessions ds ON ds.id = ? AND ds.user_id = u.id
                AND ds.device_id = d.id AND ds.revoked_at IS NULL AND ds.expires_at > ?
            WHERE u.id = ?`, claims.DeviceID, claims.SessionID, time.Now().UTC(), claims.Subject).Scan(&access)
		user, device := access.User, access.Device
		if result.Error != nil || result.RowsAffected != 1 || user.Status != model.UserActive {
			writeError(w, r, http.StatusForbidden, "account_unavailable", "This account is unavailable.", nil)
			return
		}
		if device.ID == uuid.Nil {
			writeError(w, r, http.StatusForbidden, "device_revoked", "This device has been revoked.", nil)
			return
		}
		if !access.SessionValid {
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
