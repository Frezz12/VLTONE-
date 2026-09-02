package collab

import (
	"encoding/json"
	"sort"

	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
)

// retainOperationAssetReferencesTx pins the exact immutable assets carried by
// an operation until a later canonical snapshot incorporates that sequence.
// The blob rows are locked before reference insertion so GC cannot claim a
// blob between validation and the durable reference.
func retainOperationAssetReferencesTx(tx *gorm.DB, projectID uuid.UUID,
	operationSeq int64, kind string, payload json.RawMessage) error {
	requirements, err := commandAssetRequirements(kind, payload, true)
	if err != nil {
		return err
	}
	assetIDs := make([]uuid.UUID, 0, len(requirements))
	for _, requirement := range requirements {
		assetIDs = append(assetIDs, requirement.AssetID)
	}
	assetIDs = canonicalAssetIDs(assetIDs)
	if len(assetIDs) == 0 {
		return nil
	}
	if err := retainProjectAssetIDsTx(tx, projectID, assetIDs); err != nil {
		return err
	}
	references := make([]model.ProjectOperationAsset, 0, len(assetIDs))
	for _, assetID := range assetIDs {
		references = append(references, model.ProjectOperationAsset{
			ProjectID: projectID, OperationSeq: operationSeq, AssetID: assetID,
		})
	}
	return tx.Clauses(clause.OnConflict{DoNothing: true}).
		CreateInBatches(&references, 500).Error
}

// retainSnapshotAssetReferencesTx replaces the snapshot's exact reachability
// set, then releases operation references through the materialized sequence.
// Both changes are part of the same transaction as snapshot publication.
func retainSnapshotAssetReferencesTx(tx *gorm.DB, snapshot model.ProjectSnapshot,
	manifest []string) error {
	assetIDs := make([]uuid.UUID, 0, len(manifest))
	for _, identifier := range manifest {
		assetID, err := uuid.Parse(identifier)
		if err != nil || assetID == uuid.Nil || assetID.String() != identifier {
			return ErrConflict
		}
		assetIDs = append(assetIDs, assetID)
	}
	assetIDs = canonicalAssetIDs(assetIDs)
	if len(assetIDs) != len(manifest) {
		return ErrConflict
	}
	if len(assetIDs) != 0 {
		if err := retainProjectAssetIDsTx(tx, snapshot.ProjectID, assetIDs); err != nil {
			return err
		}
		if err := tx.Where("snapshot_id = ? AND asset_id NOT IN ?", snapshot.ID, assetIDs).
			Delete(&model.ProjectSnapshotAsset{}).Error; err != nil {
			return err
		}
		references := make([]model.ProjectSnapshotAsset, 0, len(assetIDs))
		for _, assetID := range assetIDs {
			references = append(references, model.ProjectSnapshotAsset{
				SnapshotID: snapshot.ID, ProjectID: snapshot.ProjectID, AssetID: assetID,
			})
		}
		if err := tx.Clauses(clause.OnConflict{DoNothing: true}).
			CreateInBatches(&references, 500).Error; err != nil {
			return err
		}
	} else if err := tx.Where("snapshot_id = ?", snapshot.ID).
		Delete(&model.ProjectSnapshotAsset{}).Error; err != nil {
		return err
	}
	return tx.Where("project_id = ? AND operation_seq <= ?",
		snapshot.ProjectID, snapshot.Seq).Delete(&model.ProjectOperationAsset{}).Error
}

func retainProjectAssetIDsTx(tx *gorm.DB, projectID uuid.UUID,
	assetIDs []uuid.UUID) error {
	type assetBlob struct {
		AssetID uuid.UUID
		BlobID  uuid.UUID
	}
	var rows []assetBlob
	if err := tx.Raw(`SELECT assets.asset_id, assets.blob_id
		FROM project_assets AS assets
		JOIN blobs ON blobs.id = assets.blob_id
		WHERE assets.project_id = ? AND assets.asset_id IN ?
		  AND blobs.status = ? AND blobs.kind = assets.kind
		ORDER BY blobs.id, assets.asset_id
		FOR UPDATE OF blobs`, projectID, assetIDs, BlobReady).
		Scan(&rows).Error; err != nil {
		return err
	}
	if len(rows) != len(assetIDs) {
		return ErrAssetUnavailable
	}
	blobSet := make(map[uuid.UUID]struct{}, len(rows))
	for _, row := range rows {
		blobSet[row.BlobID] = struct{}{}
	}
	blobIDs := make([]uuid.UUID, 0, len(blobSet))
	for blobID := range blobSet {
		blobIDs = append(blobIDs, blobID)
	}
	sort.Slice(blobIDs, func(left, right int) bool {
		return blobIDs[left].String() < blobIDs[right].String()
	})
	updated := tx.Model(&model.Blob{}).
		Where("id IN ? AND status = ?", blobIDs, BlobReady).
		Update("unreferenced_at", nil)
	if updated.Error != nil {
		return updated.Error
	}
	if updated.RowsAffected != int64(len(blobIDs)) {
		return ErrAssetUnavailable
	}
	return nil
}

func canonicalAssetIDs(values []uuid.UUID) []uuid.UUID {
	seen := make(map[uuid.UUID]struct{}, len(values))
	result := make([]uuid.UUID, 0, len(values))
	for _, value := range values {
		if value == uuid.Nil {
			continue
		}
		if _, exists := seen[value]; exists {
			continue
		}
		seen[value] = struct{}{}
		result = append(result, value)
	}
	sort.Slice(result, func(left, right int) bool {
		return result[left].String() < result[right].String()
	})
	return result
}
