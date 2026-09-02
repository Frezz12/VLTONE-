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

// ReapExpiredUploads atomically replaces expired/aborted upload rows with exact
// provider cleanup jobs. Provider failure cannot lose the object identifiers:
// the leased cleanup queue owns every retry after the transaction commits.
func (s *AssetService) ReapExpiredUploads(ctx context.Context, limit int) (int, error) {
	if s == nil || s.DB == nil || s.Objects == nil {
		return 0, invalidf("asset maintenance is not configured")
	}
	if err := validateStorageBatch(limit); err != nil {
		return 0, err
	}
	now := s.now()
	if err := s.DB.WithContext(ctx).Model(&model.UploadSession{}).
		Where("status = ? AND verification_started_at <= ? AND expires_at > ?",
			UploadVerifying, now.Add(-30*time.Minute), now).
		Updates(map[string]any{"status": UploadUploading,
			"verification_started_at": nil}).Error; err != nil {
		return 0, err
	}
	var candidates []model.UploadSession
	cleanupIDs := make([]uuid.UUID, 0, limit)
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE", Options: "SKIP LOCKED"}).
			Where(`(status IN ? AND expires_at <= ?) OR status IN ?`,
				[]string{UploadPending, UploadUploading, UploadVerifying}, now,
				[]string{UploadExpired, UploadAborted}).
			Order("expires_at, id").Limit(limit).Find(&candidates).Error; err != nil {
			return err
		}
		for index := range candidates {
			candidate := &candidates[index]
			abortMultipart := candidate.UploadMode == UploadModeMultipart &&
				candidate.ProviderUploadID != "" && candidate.MultipartState != MultipartStateAborted
			providerUploadID := ""
			if abortMultipart {
				providerUploadID = candidate.ProviderUploadID
			}
			cleanupID, err := enqueueObjectCleanupTx(tx, candidate.ObjectKey,
				providerUploadID, abortMultipart, true, now)
			if err != nil {
				return err
			}
			cleanupIDs = append(cleanupIDs, cleanupID)
			result := tx.Where(`id = ? AND ((status IN ? AND expires_at <= ?)
				OR status IN ?)`, candidate.ID,
				[]string{UploadPending, UploadUploading, UploadVerifying}, now,
				[]string{UploadExpired, UploadAborted}).Delete(&model.UploadSession{})
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

	var cleanupErrors []error
	for _, cleanupID := range cleanupIDs {
		if err := ctx.Err(); err != nil {
			cleanupErrors = append(cleanupErrors, err)
			break
		}
		if err := s.attemptObjectCleanup(ctx, cleanupID); err != nil {
			cleanupErrors = append(cleanupErrors, err)
		}
	}
	return len(cleanupIDs), errors.Join(cleanupErrors...)
}

// RefreshUnreferencedBlobs begins the retention clock for verified blobs that
// are no longer reachable from an operation newer than the latest snapshot or
// one of the retained snapshots. The project_assets table is an immutable
// catalog, not a reachability root. Reference creation resets this field.
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
		    EXISTS (SELECT 1 FROM project_snapshots WHERE project_snapshots.blob_id = blobs.id)
		    OR EXISTS (
		      SELECT 1 FROM project_operation_assets AS refs
		      JOIN project_assets AS assets
		        ON assets.project_id = refs.project_id AND assets.asset_id = refs.asset_id
		      WHERE assets.blob_id = blobs.id
		    )
		    OR EXISTS (
		      SELECT 1 FROM project_snapshot_assets AS refs
		      JOIN project_assets AS assets
		        ON assets.project_id = refs.project_id AND assets.asset_id = refs.asset_id
		      WHERE assets.blob_id = blobs.id
		    )
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
		  AND NOT EXISTS (SELECT 1 FROM project_snapshots WHERE project_snapshots.blob_id = blobs.id)
		  AND NOT EXISTS (
		    SELECT 1 FROM project_operation_assets AS refs
		    JOIN project_assets AS assets
		      ON assets.project_id = refs.project_id AND assets.asset_id = refs.asset_id
		    WHERE assets.blob_id = blobs.id
		  )
		  AND NOT EXISTS (
		    SELECT 1 FROM project_snapshot_assets AS refs
		    JOIN project_assets AS assets
		      ON assets.project_id = refs.project_id AND assets.asset_id = refs.asset_id
		    WHERE assets.blob_id = blobs.id
		  )
		ORDER BY blobs.id
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
				AND NOT EXISTS (SELECT 1 FROM project_snapshots WHERE project_snapshots.blob_id = blobs.id)
				AND NOT EXISTS (
				  SELECT 1 FROM project_operation_assets AS refs
				  JOIN project_assets AS assets
				    ON assets.project_id = refs.project_id AND assets.asset_id = refs.asset_id
				  WHERE assets.blob_id = blobs.id
				)
				AND NOT EXISTS (
				  SELECT 1 FROM project_snapshot_assets AS refs
				  JOIN project_assets AS assets
				    ON assets.project_id = refs.project_id AND assets.asset_id = refs.asset_id
				  WHERE assets.blob_id = blobs.id
				)`,
				BlobDeleting, BlobReady, cutoff).
			Order("id").Limit(limit).
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
			// Keep the immutable catalog available for idempotent completion and
			// downloads throughout the retention window. Once GC owns the blob,
			// remove only catalog rows with no exact operation/snapshot root.
			if err := tx.Exec(`DELETE FROM project_assets AS assets
				WHERE assets.blob_id = ?
				  AND NOT EXISTS (
				    SELECT 1 FROM project_operation_assets AS refs
				    WHERE refs.project_id = assets.project_id
				      AND refs.asset_id = assets.asset_id
				  )
				  AND NOT EXISTS (
				    SELECT 1 FROM project_snapshot_assets AS refs
				    WHERE refs.project_id = assets.project_id
				      AND refs.asset_id = assets.asset_id
				  )`, blob.ID).Error; err != nil {
				return err
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
			AND NOT EXISTS (SELECT 1 FROM project_snapshots WHERE project_snapshots.blob_id = blobs.id)
			AND NOT EXISTS (
			  SELECT 1 FROM project_operation_assets AS refs
			  JOIN project_assets AS assets
			    ON assets.project_id = refs.project_id AND assets.asset_id = refs.asset_id
			  WHERE assets.blob_id = blobs.id
			)
			AND NOT EXISTS (
			  SELECT 1 FROM project_snapshot_assets AS refs
			  JOIN project_assets AS assets
			    ON assets.project_id = refs.project_id AND assets.asset_id = refs.asset_id
			  WHERE assets.blob_id = blobs.id
			)`,
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
		SELECT 1 FROM project_snapshots WHERE blob_id = ?
		UNION ALL
		SELECT 1
		FROM project_operation_assets AS refs
		JOIN project_assets AS assets
		  ON assets.project_id = refs.project_id AND assets.asset_id = refs.asset_id
		WHERE assets.blob_id = ?
		UNION ALL
		SELECT 1
		FROM project_snapshot_assets AS refs
		JOIN project_assets AS assets
		  ON assets.project_id = refs.project_id AND assets.asset_id = refs.asset_id
		WHERE assets.blob_id = ?
	)`, blobID, blobID, blobID).Scan(&referenced).Error
	return referenced, err
}
