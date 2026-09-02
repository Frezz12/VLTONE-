package collab

import (
	"context"
	"errors"
	"time"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/objectstore"
)

const (
	MinimumBlobRetention = time.Hour
	MaximumBlobRetention = 90 * 24 * time.Hour
	MaximumStorageBatch  = 1000
)

type StorageMaintenanceResult struct {
	ExpiredUploads      int
	ReferencesRefreshed int64
	DeletedBlobs        int
}

func validateStorageBatch(limit int) error {
	if limit < 1 || limit > MaximumStorageBatch {
		return invalidf("storage maintenance batch must contain between one and one thousand records")
	}
	return nil
}

// ReapExpiredUploads first makes open rows non-writable under a database lock,
// then performs provider cleanup without holding that lock. Failed cleanups
// retain an expired/aborted row and are retried on a later bounded pass.
func (s *AssetService) ReapExpiredUploads(ctx context.Context, limit int) (int, error) {
	if s == nil || s.DB == nil || s.Objects == nil {
		return 0, invalidf("asset maintenance is not configured")
	}
	if err := validateStorageBatch(limit); err != nil {
		return 0, err
	}
	now := s.now()
	var candidates []model.UploadSession
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE", Options: "SKIP LOCKED"}).
			Where(`(status IN ? AND expires_at <= ?) OR status IN ?`,
				[]string{UploadPending, UploadUploading}, now,
				[]string{UploadExpired, UploadAborted}).
			Order("expires_at, id").Limit(limit).Find(&candidates).Error; err != nil {
			return err
		}
		for index := range candidates {
			candidate := &candidates[index]
			if candidate.Status != UploadPending && candidate.Status != UploadUploading {
				continue
			}
			updates := map[string]any{"status": UploadExpired}
			candidate.Status = UploadExpired
			if candidate.UploadMode == UploadModeMultipart &&
				candidate.ProviderUploadID != "" &&
				candidate.MultipartState != MultipartStateAborted {
				updates["multipart_state"] = MultipartStateAborting
				candidate.MultipartState = MultipartStateAborting
			}
			result := tx.Model(&model.UploadSession{}).
				Where("id = ? AND status IN ?", candidate.ID,
					[]string{UploadPending, UploadUploading}).Updates(updates)
			if result.Error != nil {
				return result.Error
			}
			if result.RowsAffected != 1 {
				return ErrConflict
			}
		}
		return nil
	})
	if err != nil {
		return 0, err
	}

	removed := 0
	var cleanupErrors []error
	for _, upload := range candidates {
		if err := ctx.Err(); err != nil {
			cleanupErrors = append(cleanupErrors, err)
			break
		}
		if err := cleanupUploadObject(ctx, s.Objects, upload); err != nil {
			cleanupErrors = append(cleanupErrors, err)
			continue
		}
		result := s.DB.WithContext(ctx).Where("id = ? AND object_key = ? AND status IN ?",
			upload.ID, upload.ObjectKey, []string{UploadExpired, UploadAborted}).
			Delete(&model.UploadSession{})
		if result.Error != nil {
			cleanupErrors = append(cleanupErrors, result.Error)
			continue
		}
		if result.RowsAffected == 1 {
			removed++
		}
	}
	return removed, errors.Join(cleanupErrors...)
}

func cleanupUploadObject(ctx context.Context, objects objectstore.Store,
	upload model.UploadSession) error {
	if upload.UploadMode == UploadModeMultipart && upload.ProviderUploadID != "" &&
		upload.MultipartState != MultipartStateAborted {
		if err := objects.AbortMultipart(ctx, upload.ObjectKey,
			upload.ProviderUploadID); err != nil &&
			!errors.Is(err, objectstore.ErrMultipartNotFound) {
			return err
		}
	}
	if err := objects.Delete(ctx, upload.ObjectKey); err != nil &&
		!errors.Is(err, objectstore.ErrObjectNotFound) {
		return err
	}
	return nil
}

// RefreshUnreferencedBlobs begins the retention clock for verified blobs that
// are no longer reachable from a project asset or one of the retained
// snapshots. Reference creation resets this field through retainReadyBlobTx.
func (s *AssetService) RefreshUnreferencedBlobs(ctx context.Context,
	limit int) (int64, error) {
	if s == nil || s.DB == nil {
		return 0, invalidf("asset maintenance is not configured")
	}
	if err := validateStorageBatch(limit); err != nil {
		return 0, err
	}
	now := s.now()
	cleared := s.DB.WithContext(ctx).Exec(`WITH candidates AS (
		SELECT blobs.id
		FROM blobs
		WHERE blobs.status = ? AND blobs.unreferenced_at IS NOT NULL
		  AND (
		    EXISTS (SELECT 1 FROM project_assets WHERE project_assets.blob_id = blobs.id)
		    OR EXISTS (SELECT 1 FROM project_snapshots WHERE project_snapshots.blob_id = blobs.id)
		  )
		ORDER BY blobs.id
		FOR UPDATE SKIP LOCKED
		LIMIT ?
	)
	UPDATE blobs SET unreferenced_at = NULL
	FROM candidates WHERE blobs.id = candidates.id`, BlobReady, limit)
	if cleared.Error != nil || cleared.RowsAffected >= int64(limit) {
		return cleared.RowsAffected, cleared.Error
	}
	remaining := limit - int(cleared.RowsAffected)
	marked := s.DB.WithContext(ctx).Exec(`WITH candidates AS (
		SELECT blobs.id
		FROM blobs
		WHERE blobs.status = ? AND blobs.unreferenced_at IS NULL
		  AND NOT EXISTS (SELECT 1 FROM project_assets WHERE project_assets.blob_id = blobs.id)
		  AND NOT EXISTS (SELECT 1 FROM project_snapshots WHERE project_snapshots.blob_id = blobs.id)
		ORDER BY blobs.created_at, blobs.id
		FOR UPDATE SKIP LOCKED
		LIMIT ?
	)
	UPDATE blobs SET unreferenced_at = ?
	FROM candidates WHERE blobs.id = candidates.id`, BlobReady, remaining, now)
	return cleared.RowsAffected + marked.RowsAffected, marked.Error
}

// GarbageCollectUnreferencedBlobs claims candidates as deleting before
// touching object storage. New references require status=ready and therefore
// cannot race a successful provider deletion. A failed provider or DB delete
// leaves the durable deleting row for idempotent retry.
func (s *AssetService) GarbageCollectUnreferencedBlobs(ctx context.Context,
	retention time.Duration, limit int) (int, error) {
	if s == nil || s.DB == nil || s.Objects == nil {
		return 0, invalidf("asset maintenance is not configured")
	}
	if retention < MinimumBlobRetention || retention > MaximumBlobRetention {
		return 0, invalidf("blob retention is outside configured bounds")
	}
	if err := validateStorageBatch(limit); err != nil {
		return 0, err
	}
	cutoff := s.now().Add(-retention)
	var candidates []model.Blob
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE", Options: "SKIP LOCKED"}).
			Where(`(status = ? OR (status = ? AND unreferenced_at IS NOT NULL
				AND unreferenced_at <= ?))
				AND NOT EXISTS (SELECT 1 FROM project_assets WHERE project_assets.blob_id = blobs.id)
				AND NOT EXISTS (SELECT 1 FROM project_snapshots WHERE project_snapshots.blob_id = blobs.id)`,
				BlobDeleting, BlobReady, cutoff).
			Order("unreferenced_at NULLS FIRST, created_at, id").Limit(limit).
			Find(&candidates).Error; err != nil {
			return err
		}
		claimed := make([]model.Blob, 0, len(candidates))
		ids := make([]uuid.UUID, 0, len(candidates))
		for _, blob := range candidates {
			// This is intentionally a new SQL statement after SELECT FOR UPDATE.
			// If reference creation held the row lock first, READ COMMITTED now
			// sees its committed project_assets/project_snapshots row instead of
			// relying on the candidate statement's older MVCC snapshot.
			referenced, err := blobReferencedTx(tx, blob.ID)
			if err != nil {
				return err
			}
			if referenced {
				if blob.Status == BlobReady {
					if err := tx.Model(&model.Blob{}).Where("id = ?", blob.ID).
						Update("unreferenced_at", nil).Error; err != nil {
						return err
					}
				}
				continue
			}
			claimed = append(claimed, blob)
			if blob.Status == BlobReady {
				ids = append(ids, blob.ID)
			}
		}
		candidates = claimed
		if len(ids) == 0 {
			return nil
		}
		result := tx.Model(&model.Blob{}).
			Where("id IN ? AND status = ?", ids, BlobReady).
			Update("status", BlobDeleting)
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected != int64(len(ids)) {
			return ErrConflict
		}
		return nil
	})
	if err != nil {
		return 0, err
	}

	deleted := 0
	var cleanupErrors []error
	for _, blob := range candidates {
		if err := ctx.Err(); err != nil {
			cleanupErrors = append(cleanupErrors, err)
			break
		}
		if err := s.Objects.Delete(ctx, blob.ObjectKey); err != nil &&
			!errors.Is(err, objectstore.ErrObjectNotFound) {
			cleanupErrors = append(cleanupErrors, err)
			continue
		}
		result := s.DB.WithContext(ctx).Where(`id = ? AND status = ?
			AND NOT EXISTS (SELECT 1 FROM project_assets WHERE project_assets.blob_id = blobs.id)
			AND NOT EXISTS (SELECT 1 FROM project_snapshots WHERE project_snapshots.blob_id = blobs.id)`,
			blob.ID, BlobDeleting).Delete(&model.Blob{})
		if result.Error != nil {
			cleanupErrors = append(cleanupErrors, result.Error)
			continue
		}
		if result.RowsAffected != 1 {
			cleanupErrors = append(cleanupErrors, ErrConflict)
			continue
		}
		deleted++
	}
	return deleted, errors.Join(cleanupErrors...)
}

func blobReferencedTx(tx *gorm.DB, blobID uuid.UUID) (bool, error) {
	var referenced bool
	err := tx.Raw(`SELECT EXISTS (
		SELECT 1 FROM project_assets WHERE blob_id = ?
		UNION ALL
		SELECT 1 FROM project_snapshots WHERE blob_id = ?
	)`, blobID, blobID).Scan(&referenced).Error
	return referenced, err
}
