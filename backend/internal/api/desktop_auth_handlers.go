package api

import (
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

type desktopLoginRequest struct {
	Email       string          `json:"email"`
	Password    string          `json:"password"`
	InstallID   string          `json:"installation_id"`
	DisplayName string          `json:"device_name"`
	Platform    string          `json:"platform"`
	OSVersion   string          `json:"os_version"`
	AppVersion  string          `json:"app_version"`
	Hardware    json.RawMessage `json:"hardware"`
}

type desktopRefreshRequest struct {
	RefreshToken string `json:"refresh_token"`
	AppVersion   string `json:"app_version"`
}

// The desktop may legitimately run offline for up to 72 hours. The reporter
// must remain able to upload a crash from that same offline window; otherwise
// an otherwise valid offline session starts accumulating diagnostics that the
// server rejects after only one hour.
const reporterTokenLifetime = 72 * time.Hour

func (s *Server) desktopLogin(w http.ResponseWriter, r *http.Request) {
	var input desktopLoginRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	now := time.Now().UTC()
	key := "desktop-login:" + requestIP(r) + ":" + auth.NormalizeEmail(input.Email)
	if !s.limiter.Allow(key, 8, 15*time.Minute, now) {
		writeError(w, r, http.StatusTooManyRequests, "login_rate_limited", "Too many sign-in attempts. Try again later.", nil)
		return
	}
	user, err := findUserByEmail(s.DB, input.Email)
	if err != nil || !auth.VerifyPassword(user.PasswordHash, input.Password) {
		writeError(w, r, http.StatusUnauthorized, "invalid_credentials", "Email or password is incorrect.", nil)
		return
	}
	if user.Status != model.UserActive {
		writeError(w, r, http.StatusForbidden, "account_suspended", "This account is suspended.", nil)
		return
	}
	installID, err := uuid.Parse(strings.TrimSpace(input.InstallID))
	if err != nil || installID == uuid.Nil {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Installation identifier is invalid.", map[string]string{"installation_id": "Use a randomly generated UUID."})
		return
	}
	if input.Platform != "windows" && input.Platform != "macos" {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Platform is invalid.", map[string]string{"platform": "Supported values are windows and macos."})
		return
	}
	hardware := sanitizeHardware(input.Hardware)
	var device model.Device
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Exec("SELECT pg_advisory_xact_lock(hashtext(?))", user.ID.String()).Error; err != nil {
			return err
		}
		lookup := tx.Where("user_id = ? AND install_id = ?", user.ID, installID.String()).First(&device).Error
		if lookup != nil && !errors.Is(lookup, gorm.ErrRecordNotFound) {
			return lookup
		}
		if errors.Is(lookup, gorm.ErrRecordNotFound) || device.RevokedAt != nil {
			var active int64
			if err := tx.Model(&model.Device{}).Where("user_id = ? AND revoked_at IS NULL", user.ID).Count(&active).Error; err != nil {
				return err
			}
			if active >= 2 {
				return errDeviceLimit
			}
		}
		values := map[string]any{
			"display_name": bounded(input.DisplayName, 100), "platform": input.Platform,
			"os_version": bounded(input.OSVersion, 160), "app_version": bounded(input.AppVersion, 64),
			"hardware": hardware, "last_seen_at": now, "revoked_at": nil,
		}
		if errors.Is(lookup, gorm.ErrRecordNotFound) {
			device = model.Device{
				ID: uuid.New(), UserID: user.ID, InstallID: installID.String(),
				DisplayName: values["display_name"].(string), Platform: input.Platform,
				OSVersion: values["os_version"].(string), AppVersion: values["app_version"].(string),
				Hardware: hardware, FirstSeenAt: now, LastSeenAt: now,
			}
			return tx.Create(&device).Error
		}
		if err := tx.Model(&device).Updates(values).Error; err != nil {
			return err
		}
		return tx.First(&device, "id = ?", device.ID).Error
	})
	if errors.Is(err, errDeviceLimit) {
		writeError(w, r, http.StatusConflict, "device_limit_reached", "Demo supports two active devices. Revoke an old device in your account.", nil)
		return
	}
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "desktop_login_failed", "Desktop sign in failed.", nil)
		return
	}
	material, err := s.newDesktopSession(user.ID, device.ID, now)
	if errors.Is(err, errDesktopAccountUnavailable) {
		writeError(w, r, http.StatusForbidden, "account_unavailable",
			"This account or device is unavailable.", nil)
		return
	}
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "session_failed", "Desktop session could not be created.", nil)
		return
	}
	s.writeDesktopSession(w, r, material)
}

var (
	errDeviceLimit               = errors.New("device limit reached")
	errRefreshReuse              = errors.New("refresh token reuse")
	errRefreshInvalid            = errors.New("refresh token invalid")
	errDesktopAccountUnavailable = errors.New("desktop account unavailable")
)

type desktopSignedTokens struct {
	AccessToken      string
	AccessExpiresAt  time.Time
	OfflineToken     string
	OfflineExpiresAt time.Time
	IssuedAt         time.Time
}

type desktopSessionMaterial struct {
	User          model.User
	Device        model.Device
	Session       model.DesktopSession
	RefreshToken  string
	ReporterToken string
	Signed        desktopSignedTokens
}

func (s *Server) newDesktopSession(userID, deviceID uuid.UUID,
	now time.Time) (desktopSessionMaterial, error) {
	var material desktopSessionMaterial
	refresh, err := auth.RandomToken(48)
	if err != nil {
		return material, err
	}
	reporter, err := auth.RandomToken(40)
	if err != nil {
		return material, err
	}
	material.RefreshToken = refresh
	material.ReporterToken = reporter
	material.Session = model.DesktopSession{
		ID: uuid.New(), UserID: userID, DeviceID: deviceID,
		RefreshTokenHash: auth.HashToken(refresh), ReporterTokenHash: auth.HashToken(reporter),
		ReporterExpiresAt: now.Add(reporterTokenLifetime),
		ExpiresAt:         now.Add(30 * 24 * time.Hour), CreatedAt: now, LastSeenAt: now,
	}
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		// Credential material is signed while the exact actor rows are locked.
		// Suspend/device-revoke either commits first and this lookup fails, or
		// waits until the new DesktopSession and its entitlements commit together.
		if err := tx.Clauses(clause.Locking{Strength: "SHARE"}).
			Where("id = ? AND status = ?", userID, model.UserActive).
			First(&material.User).Error; err != nil {
			return errDesktopAccountUnavailable
		}
		if err := tx.Clauses(clause.Locking{Strength: "SHARE"}).
			Where("id = ? AND user_id = ? AND revoked_at IS NULL", deviceID, userID).
			First(&material.Device).Error; err != nil {
			return errDesktopAccountUnavailable
		}
		if err := tx.Create(&material.Session).Error; err != nil {
			return err
		}
		material.Signed, err = s.signDesktopSessionTokens(material.User,
			material.Device, material.Session, now)
		return err
	})
	return material, err
}

func (s *Server) desktopRefresh(w http.ResponseWriter, r *http.Request) {
	var input desktopRefreshRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	now := time.Now().UTC()
	var old model.DesktopSession
	err := s.DB.Where("refresh_token_hash = ?", auth.HashToken(input.RefreshToken)).First(&old).Error
	if err != nil {
		writeError(w, r, http.StatusUnauthorized, "refresh_token_invalid", "Desktop session could not be refreshed.", nil)
		return
	}
	if old.RotatedAt != nil {
		if err := s.revokeDesktopSessionsAfterRefreshReuse(old.UserID, now); err != nil {
			writeError(w, r, http.StatusInternalServerError, "session_revoke_failed",
				"Compromised desktop sessions could not be closed.", nil)
			return
		}
		writeError(w, r, http.StatusUnauthorized, "refresh_token_reused", "Refresh token reuse was detected. Sign in again.", nil)
		return
	}
	if old.RevokedAt != nil || !old.ExpiresAt.After(now) {
		writeError(w, r, http.StatusUnauthorized, "refresh_token_invalid", "Desktop session could not be refreshed.", nil)
		return
	}
	refresh, err := auth.RandomToken(48)
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "session_failed",
			"Desktop session could not be created.", nil)
		return
	}
	reporter, err := auth.RandomToken(40)
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "session_failed",
			"Desktop session could not be created.", nil)
		return
	}
	next := model.DesktopSession{
		ID: uuid.New(), UserID: old.UserID, DeviceID: old.DeviceID,
		RefreshTokenHash: auth.HashToken(refresh), ReporterTokenHash: auth.HashToken(reporter),
		ReporterExpiresAt: now.Add(reporterTokenLifetime),
		ExpiresAt:         now.Add(30 * 24 * time.Hour), CreatedAt: now, LastSeenAt: now,
	}
	var user model.User
	var device model.Device
	material := desktopSessionMaterial{
		Session: next, RefreshToken: refresh, ReporterToken: reporter,
	}
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		// Match collaboration's lock hierarchy so refresh cannot deadlock with a
		// write holding actor locks before its project lock.
		if err := tx.Clauses(clause.Locking{Strength: "SHARE"}).
			Where("id = ? AND status = ?", old.UserID, model.UserActive).
			First(&user).Error; err != nil {
			return errDesktopAccountUnavailable
		}
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("id = ? AND user_id = ? AND revoked_at IS NULL", old.DeviceID,
				old.UserID).First(&device).Error; err != nil {
			return errDesktopAccountUnavailable
		}
		var current model.DesktopSession
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("id = ? AND user_id = ? AND device_id = ?", old.ID,
				old.UserID, old.DeviceID).First(&current).Error; err != nil {
			return err
		}
		if current.RotatedAt != nil {
			return errRefreshReuse
		}
		if current.RevokedAt != nil || !current.ExpiresAt.After(now) {
			return errRefreshInvalid
		}
		old = current
		if err := tx.Model(&current).Updates(map[string]any{
			"rotated_at": now, "revoked_at": now,
		}).Error; err != nil {
			return err
		}
		if err := tx.Create(&next).Error; err != nil {
			return err
		}
		if err := tx.Model(&device).Updates(map[string]any{
			"last_seen_at": now, "app_version": bounded(input.AppVersion, 64),
		}).Error; err != nil {
			return err
		}
		if s.Collab != nil {
			if err := s.Collab.EvictDesktopSessionTx(tx, current.ID, now); err != nil {
				return err
			}
		}
		material.User = user
		material.Device = device
		material.Signed, err = s.signDesktopSessionTokens(user, device, next, now)
		return err
	})
	if errors.Is(err, errRefreshReuse) {
		if revokeErr := s.revokeDesktopSessionsAfterRefreshReuse(old.UserID, now); revokeErr != nil {
			writeError(w, r, http.StatusInternalServerError, "session_revoke_failed",
				"Compromised desktop sessions could not be closed.", nil)
			return
		}
		writeError(w, r, http.StatusUnauthorized, "refresh_token_reused", "Refresh token reuse was detected. Sign in again.", nil)
		return
	}
	if errors.Is(err, errDesktopAccountUnavailable) {
		writeError(w, r, http.StatusForbidden, "account_unavailable",
			"This account or device is unavailable.", nil)
		return
	}
	if err != nil {
		writeError(w, r, http.StatusUnauthorized, "refresh_token_invalid", "Desktop session could not be refreshed.", nil)
		return
	}
	s.disconnectCollaborationDesktopSession(old.ID, "session_rotated")
	s.writeDesktopSession(w, r, material)
}

// Refresh-token reuse is an account-level credential compromise. Revoke every
// desktop credential and evict its collaboration memberships/recording leases
// in one transaction; RoomBus connections are closed only after that commit.
// The user row is locked first to preserve collaboration's global lock order.
func (s *Server) revokeDesktopSessionsAfterRefreshReuse(userID uuid.UUID,
	now time.Time) error {
	if userID == uuid.Nil {
		return errRefreshInvalid
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		var user model.User
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			First(&user, "id = ?", userID).Error; err != nil {
			return err
		}
		if err := tx.Model(&model.DesktopSession{}).
			Where("user_id = ? AND revoked_at IS NULL", userID).
			Update("revoked_at", now).Error; err != nil {
			return err
		}
		if s.Collab != nil {
			return s.Collab.EvictUserSessionsTx(tx, userID, now)
		}
		return nil
	})
	if err != nil {
		return err
	}
	s.disconnectCollaborationUser(userID, "refresh_token_reused")
	return nil
}

func (s *Server) signDesktopSessionTokens(user model.User, device model.Device,
	session model.DesktopSession, now time.Time) (desktopSignedTokens, error) {
	if s.Signer == nil || user.ID == uuid.Nil || device.ID == uuid.Nil ||
		session.ID == uuid.Nil || session.UserID != user.ID ||
		session.DeviceID != device.ID {
		return desktopSignedTokens{}, errors.New("desktop token identity is invalid")
	}
	now = now.UTC()
	base := auth.Claims{
		Subject: user.ID, DeviceID: device.ID, SessionID: session.ID, Plan: model.PlanDemo,
		ConsentVersion: user.ConsentVersion, IssuedAt: now.Unix(),
	}
	accessClaims := base
	accessClaims.Scope = "desktop"
	accessClaims.ExpiresAt = now.Add(15 * time.Minute).Unix()
	access, err := s.Signer.Sign(accessClaims)
	if err != nil {
		return desktopSignedTokens{}, err
	}
	offlineClaims := base
	offlineClaims.Scope = "offline"
	offlineClaims.ExpiresAt = now.Add(72 * time.Hour).Unix()
	offline, err := s.Signer.Sign(offlineClaims)
	if err != nil {
		return desktopSignedTokens{}, err
	}
	return desktopSignedTokens{
		AccessToken: access, AccessExpiresAt: time.Unix(accessClaims.ExpiresAt, 0).UTC(),
		OfflineToken: offline, OfflineExpiresAt: time.Unix(offlineClaims.ExpiresAt, 0).UTC(),
		IssuedAt: now,
	}, nil
}

func (s *Server) writeDesktopSession(w http.ResponseWriter, r *http.Request,
	material desktopSessionMaterial) {
	cycle, err := s.Quota.Current(material.User.ID, material.Signed.IssuedAt)
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "quota_unavailable", "Quota is unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"access_token": material.Signed.AccessToken, "access_expires_at": material.Signed.AccessExpiresAt,
		"refresh_token": material.RefreshToken, "refresh_expires_at": material.Session.ExpiresAt,
		"reporter_token": material.ReporterToken, "reporter_expires_at": material.Session.ReporterExpiresAt,
		"offline_entitlement": material.Signed.OfflineToken,
		"offline_expires_at":  material.Signed.OfflineExpiresAt, "server_time": material.Signed.IssuedAt,
		"public_key": s.Signer.PublicKeyBase64(), "user": material.User, "device": material.Device,
		"subscription": map[string]any{"plan": "demo", "display_name": "Demo", "all_features": true},
		"quota":        quotaResponse(cycle),
	})
}

func (s *Server) desktopLogout(w http.ResponseWriter, r *http.Request) {
	claims := r.Context().Value(ctxDesktopClaims).(auth.Claims)
	now := time.Now().UTC()
	if err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Model(&model.DesktopSession{}).
			Where("id = ? AND user_id = ?", claims.SessionID, claims.Subject).
			Update("revoked_at", now).Error; err != nil {
			return err
		}
		if s.Collab != nil {
			return s.Collab.EvictDesktopSessionTx(tx, claims.SessionID, now)
		}
		return nil
	}); err != nil {
		writeError(w, r, http.StatusInternalServerError, "logout_failed",
			"Desktop session could not be closed.", nil)
		return
	}
	s.disconnectCollaborationDesktopSession(claims.SessionID, "logout")
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) desktopMe(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	cycle, err := s.Quota.Current(user.ID, time.Now().UTC())
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "quota_unavailable", "Quota is unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"user": user, "device": deviceFrom(r),
		"subscription": map[string]any{"plan": "demo", "display_name": "Demo", "all_features": true},
		"quota":        quotaResponse(cycle), "server_time": time.Now().UTC(),
	})
}

func sanitizeHardware(raw json.RawMessage) datatypes.JSON {
	var source map[string]any
	if json.Unmarshal(raw, &source) != nil {
		source = map[string]any{}
	}
	clean := make(map[string]any)
	for _, key := range []string{"cpu_model", "os_name", "os_version", "arch"} {
		if value, ok := source[key].(string); ok {
			clean[key] = redactDiagnosticText(value, 160)
		}
	}
	for _, key := range []string{"cpu_cores", "cpu_threads"} {
		if value, ok := source[key].(float64); ok && value >= 0 && value <= 4096 {
			clean[key] = int(value)
		}
	}
	if value, ok := source["ram_bytes"].(float64); ok && value >= 0 && value <= 1<<60 {
		clean["ram_bytes"] = int64(value)
	}
	if values, ok := source["gpu"].([]any); ok {
		gpus := make([]map[string]string, 0, min(len(values), 8))
		for _, rawGPU := range values {
			if len(gpus) == 8 {
				break
			}
			gpu, ok := rawGPU.(map[string]any)
			if !ok {
				continue
			}
			entry := make(map[string]string)
			for _, key := range []string{"name", "driver"} {
				if value, ok := gpu[key].(string); ok {
					entry[key] = redactDiagnosticText(value, 160)
				}
			}
			if len(entry) != 0 {
				gpus = append(gpus, entry)
			}
		}
		clean["gpu"] = gpus
	}
	body, _ := json.Marshal(clean)
	return datatypes.JSON(body)
}

func bounded(value string, max int) string {
	value = strings.TrimSpace(value)
	runes := []rune(value)
	if len(runes) > max {
		return string(runes[:max])
	}
	return value
}
