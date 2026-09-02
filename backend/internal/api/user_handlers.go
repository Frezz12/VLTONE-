package api

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"image"
	"image/jpeg"
	"image/png"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/google/uuid"
	_ "golang.org/x/image/webp"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/quota"
)

func quotaResponse(cycle model.TokenCycle) map[string]any {
	return map[string]any{
		"base_limit":       cycle.BaseLimit,
		"adjustment":       cycle.Adjustment,
		"used_tokens":      cycle.UsedTokens,
		"reserved_tokens":  cycle.ReservedTokens,
		"remaining_tokens": quota.Remaining(cycle),
		"starts_at":        cycle.StartsAt,
		"ends_at":          cycle.EndsAt,
	}
}

func (s *Server) me(w http.ResponseWriter, r *http.Request) {
	user := userFrom(r)
	session := r.Context().Value(ctxWebSession).(model.WebSession)
	s.writeUserSession(w, r, user, session.CSRFToken, http.StatusOK)
}

func (s *Server) meQuota(w http.ResponseWriter, r *http.Request) {
	cycle, err := s.Quota.Current(userFrom(r).ID, time.Now())
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "quota_unavailable", "Quota is unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, quotaResponse(cycle))
}

func (s *Server) meDevices(w http.ResponseWriter, r *http.Request) {
	var devices []model.Device
	if err := s.DB.Where("user_id = ?", userFrom(r).ID).Order("last_seen_at DESC").Find(&devices).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "devices_unavailable", "Devices are unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"devices": devices, "limit": 2})
}

func (s *Server) revokeOwnDevice(w http.ResponseWriter, r *http.Request) {
	deviceID, ok := parseUUIDParam(w, r, "deviceID")
	if !ok {
		return
	}
	now := time.Now().UTC()
	result := s.DB.Transaction(func(tx *gorm.DB) error {
		var device model.Device
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
			Where("id = ? AND user_id = ? AND revoked_at IS NULL", deviceID,
				userFrom(r).ID).First(&device).Error; err != nil {
			return err
		}
		if err := s.Collab.EvictDeviceSessionsTx(tx, deviceID, now); err != nil {
			return err
		}
		result := tx.Model(&model.Device{}).
			Where("id = ? AND user_id = ? AND revoked_at IS NULL", deviceID, userFrom(r).ID).
			Update("revoked_at", now)
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected == 0 {
			return gorm.ErrRecordNotFound
		}
		return tx.Model(&model.DesktopSession{}).
			Where("device_id = ? AND revoked_at IS NULL", deviceID).
			Update("revoked_at", now).Error
	})
	if result != nil {
		if recordNotFound(result) {
			writeError(w, r, http.StatusNotFound, "device_not_found", "Device was not found.", nil)
		} else {
			writeError(w, r, http.StatusInternalServerError, "device_revoke_failed", "Device could not be revoked.", nil)
		}
		return
	}
	s.disconnectCollaborationDevice(deviceID, "device_revoked")
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) createBugReport(w http.ResponseWriter, r *http.Request) {
	const maxFile = int64(10 << 20)
	r.Body = http.MaxBytesReader(w, r.Body, 55<<20)
	if err := r.ParseMultipartForm(55 << 20); err != nil {
		writeError(w, r, http.StatusBadRequest, "invalid_multipart", "Bug report is too large or invalid.", nil)
		return
	}
	title := strings.TrimSpace(r.FormValue("title"))
	description := strings.TrimSpace(r.FormValue("description"))
	if len([]rune(title)) < 3 || len([]rune(title)) > 160 || len([]rune(description)) < 10 || len([]rune(description)) > 20_000 {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed", "Enter a title and a detailed description.", map[string]string{
			"title":       "Title must contain 3–160 characters.",
			"description": "Description must contain 10–20000 characters.",
		})
		return
	}
	files := r.MultipartForm.File["attachments"]
	if len(files) > 5 {
		writeError(w, r, http.StatusUnprocessableEntity, "too_many_attachments", "Attach no more than five images.", nil)
		return
	}
	bug := model.BugReport{
		ID: uuid.New(), UserID: userFrom(r).ID, Title: title,
		Description: description, Steps: strings.TrimSpace(r.FormValue("steps")),
		Expected: strings.TrimSpace(r.FormValue("expected")), Actual: strings.TrimSpace(r.FormValue("actual")),
		Status: "new",
	}
	bugDir := filepath.Join(s.Config.StorageRoot, "bugs", bug.ID.String())
	if err := os.MkdirAll(bugDir, 0o700); err != nil {
		writeError(w, r, http.StatusInternalServerError, "storage_unavailable", "Attachment storage is unavailable.", nil)
		return
	}
	var attachments []model.BugAttachment
	for _, header := range files {
		body, err := multipartFile(header, maxFile)
		if err != nil || int64(len(body)) > maxFile {
			os.RemoveAll(bugDir)
			writeError(w, r, http.StatusUnprocessableEntity, "attachment_too_large", "Each image must be no larger than 10 MB.", nil)
			return
		}
		sanitized, mimeType, extension, err := sanitizeImage(body)
		if err != nil {
			os.RemoveAll(bugDir)
			writeError(w, r, http.StatusUnprocessableEntity, "attachment_invalid", "Attachments must be valid JPEG, PNG or WebP images.", nil)
			return
		}
		id := uuid.New()
		path := filepath.Join(bugDir, id.String()+extension)
		if err := os.WriteFile(path, sanitized, 0o600); err != nil {
			os.RemoveAll(bugDir)
			writeError(w, r, http.StatusInternalServerError, "attachment_write_failed", "Attachment could not be stored.", nil)
			return
		}
		digest := sha256.Sum256(sanitized)
		attachments = append(attachments, model.BugAttachment{
			ID: id, BugID: bug.ID, FileName: bounded(
				filepath.Base(strings.ReplaceAll(header.Filename, "\\", "/")), 255),
			MimeType: mimeType, Path: path, Bytes: int64(len(sanitized)),
			SHA256: hex.EncodeToString(digest[:]),
		})
	}
	if err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Create(&bug).Error; err != nil {
			return err
		}
		if len(attachments) != 0 {
			return tx.Create(&attachments).Error
		}
		return nil
	}); err != nil {
		os.RemoveAll(bugDir)
		writeError(w, r, http.StatusInternalServerError, "bug_report_failed", "Bug report could not be saved.", nil)
		return
	}
	writeJSON(w, http.StatusCreated, map[string]any{
		"id": bug.ID, "number": bug.Number, "created_at": bug.CreatedAt,
	})
}

func sanitizeImage(body []byte) ([]byte, string, string, error) {
	contentType := http.DetectContentType(body)
	if contentType != "image/jpeg" && contentType != "image/png" && contentType != "image/webp" {
		return nil, "", "", fmt.Errorf("unsupported image type %s", contentType)
	}
	configuration, _, err := image.DecodeConfig(bytes.NewReader(body))
	if err != nil || configuration.Width <= 0 || configuration.Height <= 0 ||
		configuration.Width > 12000 || configuration.Height > 12000 ||
		int64(configuration.Width)*int64(configuration.Height) > 40_000_000 {
		return nil, "", "", fmt.Errorf("invalid image dimensions")
	}
	decoded, _, err := image.Decode(bytes.NewReader(body))
	if err != nil {
		return nil, "", "", fmt.Errorf("invalid image")
	}
	var out bytes.Buffer
	if contentType == "image/png" {
		err = png.Encode(&out, decoded)
		return out.Bytes(), "image/png", ".png", err
	}
	err = jpeg.Encode(&out, decoded, &jpeg.Options{Quality: 90})
	return out.Bytes(), "image/jpeg", ".jpg", err
}

func jsonBytes(value any) json.RawMessage {
	body, _ := json.Marshal(value)
	return body
}
