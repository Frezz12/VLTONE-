package api

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"

	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/model"
)

func (s *Server) adminLogin(w http.ResponseWriter, r *http.Request) {
	var input loginRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	now := time.Now().UTC()
	if !s.limiter.Allow("admin-login:"+requestIP(r)+":"+auth.NormalizeEmail(input.Email), 5, 30*time.Minute, now) {
		writeError(w, r, http.StatusTooManyRequests, "login_rate_limited", "Too many sign-in attempts. Try again later.", nil)
		return
	}
	var admin model.AdminUser
	err := s.DB.Where("email_key = ?", auth.NormalizeEmail(input.Email)).First(&admin).Error
	if err != nil || !auth.VerifyPassword(admin.PasswordHash, input.Password) {
		writeError(w, r, http.StatusUnauthorized, "invalid_credentials", "Email or password is incorrect.", nil)
		return
	}
	if admin.Status != model.UserActive {
		writeError(w, r, http.StatusForbidden, "account_suspended", "This administrator account is suspended.", nil)
		return
	}
	raw, _ := auth.RandomToken(32)
	csrf, _ := auth.RandomToken(24)
	session := model.AdminSession{
		ID: uuid.New(), AdminUserID: admin.ID, TokenHash: auth.HashToken(raw), CSRFToken: csrf,
		LastSeenAt: now, ExpiresAt: now.Add(8 * time.Hour), CreatedAt: now,
	}
	if err := s.DB.Create(&session).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "session_failed", "Administrator sign in failed.", nil)
		return
	}
	s.setSessionCookie(w, adminCookie, raw, session.ExpiresAt)
	writeJSON(w, http.StatusOK, map[string]any{"admin": admin, "csrf_token": csrf, "expires_at": session.ExpiresAt})
}

func (s *Server) adminLogout(w http.ResponseWriter, r *http.Request) {
	session := r.Context().Value(ctxAdminSession).(model.AdminSession)
	now := time.Now().UTC()
	s.DB.Model(&session).Update("revoked_at", now)
	s.clearSessionCookie(w, adminCookie)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) adminMe(w http.ResponseWriter, r *http.Request) {
	session := r.Context().Value(ctxAdminSession).(model.AdminSession)
	writeJSON(w, http.StatusOK, map[string]any{"admin": adminFrom(r), "csrf_token": session.CSRFToken, "expires_at": session.ExpiresAt})
}

func (s *Server) adminDashboard(w http.ResponseWriter, r *http.Request) {
	now := time.Now().UTC()
	month, _ := quotaMonth(now)
	var users, activeSessions, crashes, bugs, aiUsed int64
	queries := []struct {
		target *int64
		query  *gorm.DB
	}{
		{&users, s.DB.Model(&model.User{}).Where("status = ?", model.UserActive).Count(&users)},
		{&activeSessions, s.DB.Model(&model.TelemetrySession{}).Where("last_seen_at >= ? AND ended_at IS NULL", now.Add(-10*time.Minute)).Count(&activeSessions)},
		{&crashes, s.DB.Model(&model.CrashReport{}).Where("occurred_at >= ?", now.Add(-24*time.Hour)).Count(&crashes)},
		{&bugs, s.DB.Model(&model.BugReport{}).Where("status IN ?", []string{"new", "triage", "in_progress"}).Count(&bugs)},
	}
	for _, query := range queries {
		if query.query.Error != nil {
			writeError(w, r, http.StatusInternalServerError, "dashboard_unavailable", "Dashboard data is unavailable.", nil)
			return
		}
	}
	if err := s.DB.Model(&model.TokenCycle{}).Where("starts_at = ?", month).Select("COALESCE(SUM(used_tokens), 0)").Scan(&aiUsed).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "dashboard_unavailable", "Dashboard data is unavailable.", nil)
		return
	}
	type activityPoint struct {
		Bucket   time.Time `json:"bucket"`
		Sessions int64     `json:"sessions"`
		Crashes  int64     `json:"crashes"`
	}
	type aiPoint struct {
		Bucket time.Time `json:"bucket"`
		Tokens int64     `json:"tokens"`
	}
	var activity []activityPoint
	var aiDaily []aiPoint
	if err := s.DB.Raw(`
        WITH hours AS (
            SELECT generate_series(date_trunc('hour', ?::timestamptz) - interval '23 hours',
                                   date_trunc('hour', ?::timestamptz), interval '1 hour') AS bucket)
        SELECT hours.bucket,
               (SELECT count(*) FROM telemetry_sessions s
                 WHERE s.started_at >= hours.bucket AND s.started_at < hours.bucket + interval '1 hour') AS sessions,
               (SELECT count(*) FROM crash_reports c
                 WHERE c.occurred_at >= hours.bucket AND c.occurred_at < hours.bucket + interval '1 hour') AS crashes
          FROM hours ORDER BY hours.bucket`, now, now).Scan(&activity).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "dashboard_unavailable", "Dashboard activity is unavailable.", nil)
		return
	}
	if err := s.DB.Raw(`
        WITH days AS (
            SELECT generate_series(?::date, ?::date, interval '1 day') AS bucket)
        SELECT days.bucket,
               COALESCE(SUM(CASE WHEN ledger.kind = 'usage' THEN -ledger.delta ELSE 0 END), 0)::bigint AS tokens
          FROM days
          LEFT JOIN token_ledgers ledger
            ON ledger.created_at >= days.bucket AND ledger.created_at < days.bucket + interval '1 day'
         GROUP BY days.bucket ORDER BY days.bucket`, month, now).Scan(&aiDaily).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "dashboard_unavailable", "Dashboard AI usage is unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"users": users, "active_sessions": activeSessions, "crashes_24h": crashes,
		"open_bugs": bugs, "ai_tokens_month": aiUsed, "generated_at": now,
		"activity": activity, "ai_daily": aiDaily,
	})
}

func quotaMonth(now time.Time) (time.Time, time.Time) {
	now = now.UTC()
	start := time.Date(now.Year(), now.Month(), 1, 0, 0, 0, 0, time.UTC)
	return start, start.AddDate(0, 1, 0)
}

func (s *Server) adminUsers(w http.ResponseWriter, r *http.Request) {
	query := s.DB.Model(&model.User{})
	if search := strings.TrimSpace(r.URL.Query().Get("q")); search != "" {
		like := "%" + strings.ToLower(search) + "%"
		query = query.Where("email_key LIKE ? OR nickname_key LIKE ?", like, like)
	}
	if status := strings.TrimSpace(r.URL.Query().Get("status")); status != "" {
		query = query.Where("status = ?", status)
	}
	var users []model.User
	if err := query.Order("created_at DESC").Limit(pageLimit(r)).Find(&users).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "users_unavailable", "Users are unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"users": users})
}

func (s *Server) adminUser(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	var user model.User
	if err := s.DB.First(&user, "id = ?", id).Error; err != nil {
		writeError(w, r, http.StatusNotFound, "user_not_found", "User was not found.", nil)
		return
	}
	var subscription model.Subscription
	var devices []model.Device
	var launches, crashes, bugs int64
	s.DB.Preload("Plan").Where("user_id = ?", id).First(&subscription)
	s.DB.Where("user_id = ?", id).Order("last_seen_at DESC").Find(&devices)
	s.DB.Model(&model.TelemetrySession{}).Where("user_id = ?", id).Count(&launches)
	s.DB.Model(&model.CrashReport{}).Where("user_id = ?", id).Count(&crashes)
	s.DB.Model(&model.BugReport{}).Where("user_id = ?", id).Count(&bugs)
	cycle, _ := s.Quota.Current(id, time.Now().UTC())
	writeJSON(w, http.StatusOK, map[string]any{
		"user": user, "subscription": subscription, "devices": devices, "quota": quotaResponse(cycle),
		"counts": map[string]int64{"launches": launches, "crashes": crashes, "bugs": bugs},
	})
}

func (s *Server) adminUserTelemetry(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	var sessions []model.TelemetrySession
	var samples []model.TelemetrySample
	if err := s.DB.Where("user_id = ?", id).Order("started_at DESC").Limit(pageLimit(r)).Find(&sessions).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "telemetry_unavailable", "Telemetry is unavailable.", nil)
		return
	}
	s.DB.Where("user_id = ?", id).Order("recorded_at DESC").Limit(pageLimit(r) * 5).Find(&samples)
	writeJSON(w, http.StatusOK, map[string]any{"sessions": sessions, "samples": samples})
}

func (s *Server) adminUserLedger(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	var entries []model.TokenLedger
	if err := s.DB.Where("user_id = ?", id).Order("created_at DESC").Limit(pageLimit(r)).Find(&entries).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "ledger_unavailable", "Token ledger is unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"entries": entries})
}

func (s *Server) adminSuspendUser(w http.ResponseWriter, r *http.Request) {
	s.adminSetUserStatus(w, r, model.UserSuspended)
}
func (s *Server) adminActivateUser(w http.ResponseWriter, r *http.Request) {
	s.adminSetUserStatus(w, r, model.UserActive)
}

func (s *Server) adminSetUserStatus(w http.ResponseWriter, r *http.Request, status string) {
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	now := time.Now().UTC()
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if result := tx.Model(&model.User{}).Where("id = ?", id).Update("status", status); result.Error != nil || result.RowsAffected == 0 {
			if result.Error != nil {
				return result.Error
			}
			return gorm.ErrRecordNotFound
		}
		if status == model.UserSuspended {
			tx.Model(&model.WebSession{}).Where("user_id = ? AND revoked_at IS NULL", id).Update("revoked_at", now)
			tx.Model(&model.DesktopSession{}).Where("user_id = ? AND revoked_at IS NULL", id).Update("revoked_at", now)
		}
		return s.audit(tx, r, "user."+status, "user", id, map[string]any{})
	})
	if err != nil {
		writeError(w, r, http.StatusNotFound, "user_update_failed", "User could not be updated.", nil)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

type tokenAddInput struct {
	Amount int64 `json:"amount"`
}
type passwordConfirmInput struct {
	Password string `json:"password"`
}

func (s *Server) adminAddTokens(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	var input tokenAddInput
	if !decodeJSON(w, r, &input) {
		return
	}
	cycle, err := s.Quota.Adjust(id, adminFrom(r).ID, input.Amount, time.Now().UTC())
	if err != nil {
		writeError(w, r, http.StatusUnprocessableEntity, "token_adjustment_failed", "Token adjustment failed.", nil)
		return
	}
	s.audit(s.DB, r, "tokens.add", "user", id, map[string]any{"amount": input.Amount})
	writeJSON(w, http.StatusOK, quotaResponse(cycle))
}

func (s *Server) adminResetTokens(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	var input passwordConfirmInput
	if !decodeJSON(w, r, &input) {
		return
	}
	if !s.confirmAdminPassword(r, input.Password) {
		writeError(w, r, http.StatusUnauthorized, "reauthentication_failed", "Administrator password is incorrect.", nil)
		return
	}
	cycle, err := s.Quota.Reset(id, adminFrom(r).ID, time.Now().UTC())
	if err != nil {
		writeError(w, r, http.StatusConflict, "token_reset_failed", "Token cycle could not be reset while requests are active.", nil)
		return
	}
	s.audit(s.DB, r, "tokens.reset", "user", id, map[string]any{})
	writeJSON(w, http.StatusOK, quotaResponse(cycle))
}

func (s *Server) adminRevokeDevice(w http.ResponseWriter, r *http.Request) {
	userID, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	deviceID, ok := parseUUIDParam(w, r, "deviceID")
	if !ok {
		return
	}
	now := time.Now().UTC()
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		result := tx.Model(&model.Device{}).Where("id = ? AND user_id = ? AND revoked_at IS NULL", deviceID, userID).Update("revoked_at", now)
		if result.Error != nil || result.RowsAffected == 0 {
			return gorm.ErrRecordNotFound
		}
		tx.Model(&model.DesktopSession{}).Where("device_id = ? AND revoked_at IS NULL", deviceID).Update("revoked_at", now)
		return s.audit(tx, r, "device.revoke", "user", userID, map[string]any{"device_hash": targetHash(deviceID)})
	})
	if err != nil {
		writeError(w, r, http.StatusNotFound, "device_not_found", "Device was not found.", nil)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) adminRevokeSessions(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	now := time.Now().UTC()
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		tx.Model(&model.WebSession{}).Where("user_id = ? AND revoked_at IS NULL", id).Update("revoked_at", now)
		tx.Model(&model.DesktopSession{}).Where("user_id = ? AND revoked_at IS NULL", id).Update("revoked_at", now)
		return s.audit(tx, r, "sessions.revoke", "user", id, map[string]any{})
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "session_revoke_failed", "Sessions could not be revoked.", nil)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) adminDeleteDiagnostics(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	bugPaths, crashPaths := s.diagnosticPaths(id)
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Where("user_id = ?", id).Delete(&model.TelemetrySession{}).Error; err != nil {
			return err
		}
		if err := tx.Where("user_id = ?", id).Delete(&model.CrashReport{}).Error; err != nil {
			return err
		}
		if err := tx.Where("user_id = ?", id).Delete(&model.BugReport{}).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "diagnostics.delete", "user", id, map[string]any{})
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "diagnostics_delete_failed", "Diagnostics could not be deleted.", nil)
		return
	}
	removeStoredPaths(append(bugPaths, crashPaths...))
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) adminDeleteUser(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "userID")
	if !ok {
		return
	}
	var input passwordConfirmInput
	if !decodeJSON(w, r, &input) {
		return
	}
	if !s.confirmAdminPassword(r, input.Password) {
		writeError(w, r, http.StatusUnauthorized, "reauthentication_failed", "Administrator password is incorrect.", nil)
		return
	}
	bugPaths, crashPaths := s.diagnosticPaths(id)
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := s.audit(tx, r, "user.delete", "user", id, map[string]any{"scope": "full"}); err != nil {
			return err
		}
		result := tx.Delete(&model.User{}, "id = ?", id)
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected == 0 {
			return gorm.ErrRecordNotFound
		}
		return nil
	})
	if err != nil {
		writeError(w, r, http.StatusNotFound, "user_delete_failed", "User could not be deleted.", nil)
		return
	}
	removeStoredPaths(append(bugPaths, crashPaths...))
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) adminBugs(w http.ResponseWriter, r *http.Request) {
	var bugs []model.BugReport
	query := s.DB.Order("created_at DESC").Limit(pageLimit(r))
	if status := r.URL.Query().Get("status"); status != "" {
		query = query.Where("status = ?", status)
	}
	if err := query.Find(&bugs).Error; err != nil {
		writeError(w, r, 500, "bugs_unavailable", "Bug reports are unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"bugs": bugs})
}

type bugUpdateInput struct {
	Status       string `json:"status"`
	InternalNote string `json:"internal_note"`
}

func (s *Server) adminUpdateBug(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "bugID")
	if !ok {
		return
	}
	var input bugUpdateInput
	if !decodeJSON(w, r, &input) {
		return
	}
	allowed := map[string]bool{"new": true, "triage": true, "in_progress": true, "fixed": true, "duplicate": true, "wont_fix": true}
	if !allowed[input.Status] {
		writeError(w, r, 422, "validation_failed", "Bug status is invalid.", nil)
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		result := tx.Model(&model.BugReport{}).Where("id = ?", id).Updates(map[string]any{"status": input.Status, "internal_note": bounded(input.InternalNote, 20_000), "updated_at": time.Now().UTC()})
		if result.Error != nil || result.RowsAffected == 0 {
			return gorm.ErrRecordNotFound
		}
		return s.audit(tx, r, "bug.update", "bug", id, map[string]any{"status": input.Status})
	})
	if err != nil {
		writeError(w, r, 404, "bug_not_found", "Bug report was not found.", nil)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) adminBugAttachment(w http.ResponseWriter, r *http.Request) {
	bugID, ok := parseUUIDParam(w, r, "bugID")
	if !ok {
		return
	}
	attachmentID, ok := parseUUIDParam(w, r, "attachmentID")
	if !ok {
		return
	}
	var item model.BugAttachment
	if s.DB.Where("id = ? AND bug_id = ?", attachmentID, bugID).First(&item).Error != nil {
		writeError(w, r, 404, "attachment_not_found", "Attachment was not found.", nil)
		return
	}
	w.Header().Set("Content-Type", item.MimeType)
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", filepath.Base(item.FileName)))
	http.ServeFile(w, r, item.Path)
}

func (s *Server) adminCrashes(w http.ResponseWriter, r *http.Request) {
	var crashes []model.CrashReport
	if err := s.DB.Order("occurred_at DESC").Limit(pageLimit(r)).Find(&crashes).Error; err != nil {
		writeError(w, r, 500, "crashes_unavailable", "Crash reports are unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"crashes": crashes})
}

func (s *Server) adminCrashArtifact(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "crashID")
	if !ok {
		return
	}
	var crash model.CrashReport
	if s.DB.First(&crash, "id = ?", id).Error != nil || crash.ArtifactPath == "" {
		writeError(w, r, 404, "artifact_not_found", "Crash artifact was not found.", nil)
		return
	}
	extension := strings.ToLower(filepath.Ext(crash.ArtifactPath))
	contentType := "application/octet-stream"
	if extension == ".log" {
		contentType = "text/plain; charset=utf-8"
	} else if extension == ".json" {
		contentType = "application/json"
	} else {
		extension = ".bin"
	}
	w.Header().Set("Content-Type", contentType)
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", "vlt-crash-"+id.String()+extension))
	w.Header().Set("X-Content-SHA256", crash.SHA256)
	http.ServeFile(w, r, crash.ArtifactPath)
}

func (s *Server) adminAudit(w http.ResponseWriter, r *http.Request) {
	var entries []model.AdminAuditLog
	if err := s.DB.Order("created_at DESC").Limit(pageLimit(r)).Find(&entries).Error; err != nil {
		writeError(w, r, 500, "audit_unavailable", "Audit log is unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"entries": entries})
}

func (s *Server) confirmAdminPassword(r *http.Request, password string) bool {
	return password != "" && auth.VerifyPassword(adminFrom(r).PasswordHash, password)
}

func (s *Server) audit(tx *gorm.DB, r *http.Request, action, targetType string, target uuid.UUID, metadata map[string]any) error {
	body, _ := json.Marshal(metadata)
	return tx.Create(&model.AdminAuditLog{
		ID: uuid.New(), AdminUserID: adminFrom(r).ID, Action: action, TargetType: targetType,
		TargetHash: targetHash(target), Metadata: datatypes.JSON(body), IP: requestIP(r), CreatedAt: time.Now().UTC(),
	}).Error
}

func (s *Server) diagnosticPaths(userID uuid.UUID) ([]string, []string) {
	var bugs []string
	var crashes []string
	s.DB.Model(&model.BugAttachment{}).Select("bug_attachments.path").
		Joins("JOIN bug_reports ON bug_reports.id = bug_attachments.bug_id").Where("bug_reports.user_id = ?", userID).Scan(&bugs)
	s.DB.Model(&model.CrashReport{}).Where("user_id = ? AND artifact_path <> ''", userID).Pluck("artifact_path", &crashes)
	return bugs, crashes
}

func removeStoredPaths(paths []string) {
	for _, path := range paths {
		if path != "" {
			_ = os.RemoveAll(filepath.Dir(path))
		}
	}
}
