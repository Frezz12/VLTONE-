package collab

import (
	"context"
	"errors"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/objectstore"
)

const (
	objectCleanupPending  = "pending"
	objectCleanupRunning  = "running"
	objectCleanupLease    = 5 * time.Minute
	objectCleanupRetry    = 5 * time.Second
	objectCleanupMaxRetry = time.Hour
)

func enqueueObjectCleanupTx(tx *gorm.DB, objectKey, providerUploadID string,
	abortMultipart, deleteObject bool, now time.Time) (uuid.UUID, error) {
	if tx == nil || !validCleanupObjectKey(objectKey) ||
		(!abortMultipart && !deleteObject) ||
		(abortMultipart && !validCleanupProviderID(providerUploadID)) ||
		(!abortMultipart && providerUploadID != "") {
		return uuid.Nil, invalidf("object cleanup job is invalid")
	}
	job := model.ObjectCleanupJob{
		ID: uuid.New(), ObjectKey: objectKey, ProviderUploadID: providerUploadID,
		AbortMultipart: abortMultipart, DeleteObject: deleteObject,
		Status: objectCleanupPending, RetryAvailableAt: now,
		CreatedAt: now, UpdatedAt: now,
	}
	if err := tx.Clauses(clause.OnConflict{
		Columns: []clause.Column{
			{Name: "object_key"}, {Name: "provider_upload_id"},
			{Name: "abort_multipart"}, {Name: "delete_object"},
		},
		DoNothing: true,
	}).Create(&job).Error; err != nil {
		return uuid.Nil, err
	}
	var persisted model.ObjectCleanupJob
	if err := tx.Where("object_key = ? AND provider_upload_id = ? AND abort_multipart = ? AND delete_object = ?",
		objectKey, providerUploadID, abortMultipart, deleteObject).
		First(&persisted).Error; err != nil {
		return uuid.Nil, err
	}
	return persisted.ID, nil
}

func validCleanupObjectKey(value string) bool {
	return len(value) >= len("uploads/x") && len(value) <= 1024 &&
		utf8.ValidString(value) && strings.HasPrefix(value, "uploads/") &&
		!strings.ContainsAny(value, "\x00\r\n")
}

func validCleanupProviderID(value string) bool {
	if value == "" || len(value) > 2048 || !utf8.ValidString(value) {
		return false
	}
	for _, character := range value {
		if character < 0x20 || character == 0x7f {
			return false
		}
	}
	return true
}

// DrainObjectCleanupJobs claims jobs one at a time so a shutdown can strand at
// most one provider operation. An expired lease is safe to reclaim after a
// crash because both S3 actions accept already-missing provider state.
func (s *AssetService) DrainObjectCleanupJobs(ctx context.Context,
	limit int) (int, error) {
	if s == nil || s.DB == nil || s.Objects == nil {
		return 0, invalidf("asset maintenance is not configured")
	}
	if err := validateStorageBatch(limit); err != nil {
		return 0, err
	}
	completed := 0
	var cleanupErrors []error
	for range limit {
		if err := ctx.Err(); err != nil {
			cleanupErrors = append(cleanupErrors, err)
			break
		}
		job, found, err := s.claimObjectCleanupJob(ctx, uuid.Nil, false)
		if err != nil {
			cleanupErrors = append(cleanupErrors, err)
			break
		}
		if !found {
			break
		}
		if err := s.executeObjectCleanupJob(ctx, job); err != nil {
			cleanupErrors = append(cleanupErrors, err)
			continue
		}
		completed++
	}
	return completed, errors.Join(cleanupErrors...)
}

func (s *AssetService) ObjectCleanupQueueDepth(ctx context.Context) (int64, int64, error) {
	if s == nil || s.DB == nil {
		return 0, 0, invalidf("asset maintenance is not configured")
	}
	var rows []struct {
		Status string
		Count  int64
	}
	if err := s.DB.WithContext(ctx).Model(&model.ObjectCleanupJob{}).
		Select("status, count(*) AS count").Group("status").Scan(&rows).Error; err != nil {
		return 0, 0, err
	}
	var pending, running int64
	for _, row := range rows {
		switch row.Status {
		case objectCleanupPending:
			pending = row.Count
		case objectCleanupRunning:
			running = row.Count
		}
	}
	return pending, running, nil
}

func (s *AssetService) attemptObjectCleanup(ctx context.Context, id uuid.UUID) error {
	if id == uuid.Nil {
		return nil
	}
	job, found, err := s.claimObjectCleanupJob(ctx, id, true)
	if err != nil || !found {
		return err
	}
	return s.executeObjectCleanupJob(ctx, job)
}

func (s *AssetService) claimObjectCleanupJob(ctx context.Context, id uuid.UUID,
	force bool) (model.ObjectCleanupJob, bool, error) {
	now := s.now()
	var claimed model.ObjectCleanupJob
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		query := tx.Clauses(clause.Locking{Strength: "UPDATE", Options: "SKIP LOCKED"})
		if id != uuid.Nil {
			query = query.Where("id = ?", id)
		}
		if force {
			query = query.Where(`status = ? OR
				(status = ? AND lease_expires_at <= ?)`,
				objectCleanupPending, objectCleanupRunning, now)
		} else {
			query = query.Where(`(status = ? AND retry_available_at <= ?) OR
				(status = ? AND lease_expires_at <= ?)`,
				objectCleanupPending, now, objectCleanupRunning, now)
		}
		if err := query.Order("retry_available_at, created_at, id").
			First(&claimed).Error; err != nil {
			return err
		}
		token := uuid.New()
		lease := now.Add(objectCleanupLease)
		result := tx.Model(&model.ObjectCleanupJob{}).Where("id = ?", claimed.ID).
			Updates(map[string]any{
				"status": objectCleanupRunning, "attempt_count": gorm.Expr("attempt_count + 1"),
				"claim_token": token, "lease_expires_at": lease,
				"last_error": "", "updated_at": now,
			})
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected != 1 {
			return ErrConflict
		}
		claimed.Status = objectCleanupRunning
		claimed.AttemptCount++
		claimed.ClaimToken = &token
		claimed.LeaseExpiresAt = &lease
		return nil
	})
	if errors.Is(err, gorm.ErrRecordNotFound) {
		return model.ObjectCleanupJob{}, false, nil
	}
	return claimed, err == nil, err
}

func (s *AssetService) executeObjectCleanupJob(ctx context.Context,
	job model.ObjectCleanupJob) error {
	if job.ClaimToken == nil {
		return ErrConflict
	}
	providerErr := cleanupQueuedObject(ctx, s.Objects, job)
	now := s.now()
	if providerErr == nil {
		result := s.DB.WithContext(ctx).Where("id = ? AND status = ? AND claim_token = ?",
			job.ID, objectCleanupRunning, *job.ClaimToken).Delete(&model.ObjectCleanupJob{})
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected != 1 {
			return ErrConflict
		}
		return nil
	}
	result := s.DB.WithContext(ctx).Model(&model.ObjectCleanupJob{}).
		Where("id = ? AND status = ? AND claim_token = ?", job.ID,
			objectCleanupRunning, *job.ClaimToken).
		Updates(map[string]any{
			"status": objectCleanupPending, "claim_token": nil, "lease_expires_at": nil,
			"retry_available_at": now.Add(objectCleanupRetryDelay(job.AttemptCount)),
			"last_error":         cleanupErrorText(providerErr), "updated_at": now,
		})
	if result.Error != nil {
		return errors.Join(providerErr, result.Error)
	}
	if result.RowsAffected != 1 {
		return errors.Join(providerErr, ErrConflict)
	}
	return providerErr
}

func cleanupQueuedObject(ctx context.Context, objects objectstore.Store,
	job model.ObjectCleanupJob) error {
	if job.AbortMultipart {
		if err := objects.AbortMultipart(ctx, job.ObjectKey,
			job.ProviderUploadID); err != nil &&
			!errors.Is(err, objectstore.ErrMultipartNotFound) {
			return err
		}
	}
	if job.DeleteObject {
		if err := objects.Delete(ctx, job.ObjectKey); err != nil &&
			!errors.Is(err, objectstore.ErrObjectNotFound) {
			return err
		}
	}
	return nil
}

func objectCleanupRetryDelay(attempt int) time.Duration {
	if attempt < 1 {
		return objectCleanupRetry
	}
	shift := attempt - 1
	if shift > 10 {
		shift = 10
	}
	delay := objectCleanupRetry * time.Duration(1<<shift)
	if delay > objectCleanupMaxRetry {
		return objectCleanupMaxRetry
	}
	return delay
}

func cleanupErrorText(err error) string {
	if err == nil {
		return ""
	}
	cleaned := strings.Map(func(character rune) rune {
		if character < 0x20 || character == 0x7f {
			return ' '
		}
		return character
	}, err.Error())
	runes := []rune(strings.TrimSpace(cleaned))
	if len(runes) > 512 {
		runes = runes[:512]
	}
	return string(runes)
}
