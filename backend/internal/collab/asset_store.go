package collab

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"path"
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/objectstore"
)

const (
	UploadPending   = "pending"
	UploadUploading = "uploading"
	UploadCompleted = "completed"
	UploadAborted   = "aborted"
	UploadExpired   = "expired"

	BlobPending     = "pending"
	BlobReady       = "ready"
	BlobQuarantined = "quarantined"
	BlobDeleting    = "deleting"

	DefaultUploadURLTTL            = 15 * time.Minute
	DefaultDownloadURLTTL          = 5 * time.Minute
	DefaultUploadSessionTTL        = 24 * time.Hour
	DefaultMultipartThresholdBytes = 64 << 20
	DefaultMultipartPartBytes      = 16 << 20
	DefaultMultipartURLBatch       = 100
	MaximumSnapshotAssets          = 100000
)

var (
	ErrUploadExpired    = errors.New("collaboration upload session has expired")
	ErrUploadState      = errors.New("collaboration upload session cannot be changed")
	ErrAssetUnavailable = errors.New("collaboration asset is not verified and ready")
)

type AssetService struct {
	DB                      *gorm.DB
	Objects                 objectstore.Store
	Now                     func() time.Time
	UploadURLTTL            time.Duration
	DownloadURLTTL          time.Duration
	UploadSessionTTL        time.Duration
	MaximumBytes            int64
	MultipartThresholdBytes int64
	MultipartPartBytes      int64
	MultipartURLBatch       int
}

func NewAssetService(db *gorm.DB, objects objectstore.Store, uploadURLTTL,
	downloadURLTTL, uploadSessionTTL time.Duration, maximumBytes,
	multipartThresholdBytes, multipartPartBytes int64, multipartURLBatch int) *AssetService {
	return &AssetService{
		DB: db, Objects: objects, Now: func() time.Time { return time.Now().UTC() },
		UploadURLTTL: uploadURLTTL, DownloadURLTTL: downloadURLTTL,
		UploadSessionTTL: uploadSessionTTL, MaximumBytes: maximumBytes,
		MultipartThresholdBytes: multipartThresholdBytes,
		MultipartPartBytes:      multipartPartBytes, MultipartURLBatch: multipartURLBatch,
	}
}

func (s *AssetService) now() time.Time {
	if s.Now != nil {
		return s.Now().UTC()
	}
	return time.Now().UTC()
}

func (s *AssetService) normalizedDurations() (time.Duration, time.Duration, time.Duration) {
	uploadURL := s.UploadURLTTL
	if uploadURL <= 0 {
		uploadURL = DefaultUploadURLTTL
	}
	downloadURL := s.DownloadURLTTL
	if downloadURL <= 0 {
		downloadURL = DefaultDownloadURLTTL
	}
	session := s.UploadSessionTTL
	if session <= 0 {
		session = DefaultUploadSessionTTL
	}
	return uploadURL, downloadURL, session
}

func (s *AssetService) normalizedMultipartConfig() (int64, int64, int) {
	threshold := s.MultipartThresholdBytes
	if threshold <= 0 {
		threshold = DefaultMultipartThresholdBytes
	}
	partBytes := s.MultipartPartBytes
	if partBytes <= 0 {
		partBytes = DefaultMultipartPartBytes
	}
	batch := s.MultipartURLBatch
	if batch <= 0 || batch > 200 {
		batch = DefaultMultipartURLBatch
	}
	return threshold, partBytes, batch
}

type PrepareAssetUploadInput struct {
	ProjectID       uuid.UUID
	UploadID        uuid.UUID
	AssetID         uuid.UUID
	ActorUserID     uuid.UUID
	DeviceID        uuid.UUID
	ActorSessionID  uuid.UUID
	SHA256          string
	Bytes           int64
	Kind            string
	ContentType     string
	DisplayName     string
	PartNumberStart int
}

type PrepareSnapshotUploadInput struct {
	ProjectID       uuid.UUID
	UploadID        uuid.UUID
	ActorUserID     uuid.UUID
	DeviceID        uuid.UUID
	ActorSessionID  uuid.UUID
	Seq             int64
	SchemaVersion   int
	SHA256          string
	Bytes           int64
	ContentType     string
	AssetIDs        []string
	PartNumberStart int
}

type CompleteMultipartPart struct {
	PartNumber int    `json:"partNumber"`
	ETag       string `json:"eTag"`
}

type PreparedMultipartPart struct {
	PartNumber int                          `json:"partNumber"`
	ByteSize   int64                        `json:"byteSize"`
	Request    objectstore.PresignedRequest `json:"request"`
}

type UploadedMultipartPart struct {
	PartNumber int    `json:"partNumber"`
	ByteSize   int64  `json:"byteSize"`
	ETag       string `json:"eTag"`
}

type UploadPreparation struct {
	UploadID            uuid.UUID                     `json:"uploadId"`
	AssetID             *uuid.UUID                    `json:"assetId,omitempty"`
	SnapshotSeq         *int64                        `json:"snapshotSeq,omitempty"`
	Status              string                        `json:"status"`
	UploadMode          string                        `json:"uploadMode"`
	AlreadyAvailable    bool                          `json:"alreadyAvailable"`
	Request             *objectstore.PresignedRequest `json:"request,omitempty"`
	MultipartPartSize   *int64                        `json:"multipartPartSize,omitempty"`
	MultipartPartCount  *int                          `json:"multipartPartCount,omitempty"`
	UploadedParts       []UploadedMultipartPart       `json:"uploadedParts,omitempty"`
	Parts               []PreparedMultipartPart       `json:"parts,omitempty"`
	NextPartNumberStart *int                          `json:"nextPartNumberStart,omitempty"`
	ExpiresAt           time.Time                     `json:"expiresAt"`
	// Finalization is server-control metadata used after an idempotent snapshot
	// dedupe closes an ending session. It is never serialized to the desktop.
	Finalization *SnapshotFinalization `json:"-"`
}

type CompletedAsset struct {
	Asset model.ProjectAsset `json:"asset"`
	Blob  model.Blob         `json:"blob"`
}

type CompletedSnapshot struct {
	Snapshot model.ProjectSnapshot `json:"snapshot"`
	Blob     model.Blob            `json:"blob"`
	// Finalization is emitted only by the transaction that changes an ending
	// live session to ended. API retries therefore cannot duplicate teardown.
	Finalization *SnapshotFinalization `json:"-"`
}

type PresignedDownload struct {
	Request     objectstore.PresignedRequest `json:"request"`
	SHA256      string                       `json:"sha256"`
	Bytes       int64                        `json:"byteSize"`
	ContentType string                       `json:"contentType"`
}

type normalizedUpload struct {
	UploadID              uuid.UUID
	AssetID               *uuid.UUID
	SnapshotSeq           *int64
	SnapshotSchemaVersion *int
	SnapshotAssetIDs      datatypes.JSON
	ProjectID             uuid.UUID
	ActorUserID           uuid.UUID
	DeviceID              uuid.UUID
	ActorSessionID        uuid.UUID
	SHA256                string
	Bytes                 int64
	Kind                  string
	ContentType           string
	DisplayName           string
	RequestHash           string
}

func (s *AssetService) PrepareAssetUpload(ctx context.Context,
	input PrepareAssetUploadInput) (UploadPreparation, error) {
	normalized, err := s.normalizeAssetUpload(input)
	if err != nil {
		return UploadPreparation{}, err
	}
	upload, _, err := s.prepareUpload(ctx, normalized)
	if err != nil {
		return UploadPreparation{}, err
	}
	upload, err = s.ensureMultipartCreated(ctx, upload, normalized)
	if err != nil {
		return UploadPreparation{}, err
	}
	return s.uploadPreparation(ctx, upload, input.PartNumberStart, normalized)
}

func (s *AssetService) PrepareSnapshotUpload(ctx context.Context,
	input PrepareSnapshotUploadInput) (UploadPreparation, error) {
	normalized, err := s.normalizeSnapshotUpload(input)
	if err != nil {
		return UploadPreparation{}, err
	}
	upload, finalization, err := s.prepareUpload(ctx, normalized)
	if err != nil {
		return UploadPreparation{}, err
	}
	upload, err = s.ensureMultipartCreated(ctx, upload, normalized)
	if err != nil {
		return UploadPreparation{}, err
	}
	preparation, err := s.uploadPreparation(ctx, upload, input.PartNumberStart, normalized)
	preparation.Finalization = finalization
	return preparation, err
}

func (s *AssetService) prepareUpload(ctx context.Context,
	normalized normalizedUpload) (model.UploadSession, *SnapshotFinalization, error) {
	var result model.UploadSession
	var finalization *SnapshotFinalization
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.lockActiveActorTx(tx, normalized.ActorUserID,
			normalized.DeviceID, normalized.ActorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, normalized.ProjectID, normalized.ActorUserID, true)
		if err != nil {
			return err
		}
		var existing model.UploadSession
		lookup := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&existing, "id = ?", normalized.UploadID)
		if lookup.Error == nil {
			if existing.ProjectID != normalized.ProjectID || existing.CreatedBy != normalized.ActorUserID ||
				existing.DeviceID == nil || *existing.DeviceID != normalized.DeviceID ||
				!uploadRequestMatches(existing, normalized) ||
				!snapshotUploadManifestMatches(existing, normalized) {
				return ErrConflict
			}
			if normalized.Kind == "project_snapshot" {
				if err := requireSnapshotAssetsReadyTx(tx, normalized.ProjectID,
					normalized.SnapshotAssetIDs); err != nil {
					return err
				}
			}
			// A retry for an unfinished upload can mint a fresh presigned PUT URL,
			// so it must repeat the current role/session authorization instead of
			// relying on the permissions held when the row was first created.
			if existing.Status != UploadCompleted {
				if err := s.authorizeUploadTx(tx, view, normalized.ActorUserID,
					normalized.DeviceID, normalized.ActorSessionID,
					normalized.Kind == "project_snapshot"); err != nil {
					return err
				}
				if normalized.Kind == "project_snapshot" &&
					(normalized.SnapshotSeq == nil || *normalized.SnapshotSeq != view.Project.HeadSeq) {
					return ErrConflict
				}
				if _, err := pendingSnapshotForUploadTx(tx, view.Project, normalized,
					true); err != nil {
					return err
				}
			}
			result = existing
			return nil
		}
		if !errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
			return lookup.Error
		}
		if err := s.authorizeUploadTx(tx, view, normalized.ActorUserID,
			normalized.DeviceID, normalized.ActorSessionID,
			normalized.Kind == "project_snapshot"); err != nil {
			return err
		}
		if normalized.Kind == "project_snapshot" &&
			(normalized.SnapshotSeq == nil || *normalized.SnapshotSeq != view.Project.HeadSeq) {
			return ErrConflict
		}
		request, err := pendingSnapshotForUploadTx(tx, view.Project, normalized, true)
		if err != nil {
			return err
		}
		if normalized.Kind == "project_snapshot" {
			if err := requireSnapshotAssetsReadyTx(tx, normalized.ProjectID,
				normalized.SnapshotAssetIDs); err != nil {
				return err
			}
		}

		now := s.now()
		upload := model.UploadSession{
			ID: normalized.UploadID, ProjectID: normalized.ProjectID,
			AssetID: normalized.AssetID, CreatedBy: normalized.ActorUserID,
			DeviceID: &normalized.DeviceID, ExpectedSHA256: normalized.SHA256,
			ExpectedBytes: normalized.Bytes, Kind: normalized.Kind,
			SnapshotSeq:           normalized.SnapshotSeq,
			SnapshotSchemaVersion: normalized.SnapshotSchemaVersion,
			SnapshotAssetIDs:      storedSnapshotAssetManifest(normalized.SnapshotAssetIDs),
			ContentType:           normalized.ContentType, DisplayName: normalized.DisplayName,
			RequestHash: normalized.RequestHash,
			ObjectKey:   stagingObjectKey(normalized.ProjectID, normalized.UploadID),
			Status:      UploadUploading, CreatedAt: now,
			UploadMode: UploadModeSingle, MultipartState: MultipartStateNone,
			MultipartManifest: datatypes.JSON([]byte("[]")),
		}
		_, _, sessionTTL := s.normalizedDurations()
		upload.ExpiresAt = now.Add(sessionTTL)

		blob, available, err := s.findDedupeBlobTx(tx, normalized)
		if err != nil {
			return err
		}
		if available {
			if normalized.Kind == "project_snapshot" {
				snapshot, err := s.linkSnapshotTx(tx, view.Project, upload, blob, true)
				if err != nil {
					return err
				}
				if err := retainLatestSnapshotsTx(tx, view.Project.ID, 2); err != nil {
					return err
				}
				if request != nil {
					finalization, err = completeSnapshotRequestTx(tx, *request,
						snapshot, now)
					if err != nil {
						return err
					}
				}
			} else if _, err := s.linkAssetTx(tx, upload, blob); err != nil {
				return err
			}
			upload.BlobID = &blob.ID
			upload.ObjectKey = blob.ObjectKey
			upload.Status = UploadCompleted
			upload.CompletedAt = &now
		} else {
			threshold, preferredPartBytes, _ := s.normalizedMultipartConfig()
			plan, err := planMultipart(upload.ExpectedBytes, threshold, preferredPartBytes)
			if err != nil {
				return err
			}
			if plan.Enabled {
				partSize, partCount := plan.PartSize, plan.Parts
				upload.UploadMode = UploadModeMultipart
				upload.MultipartPartSize = &partSize
				upload.MultipartPartCount = &partCount
				upload.MultipartState = MultipartStateCreating
			}
		}
		if err := tx.Create(&upload).Error; err != nil {
			return err
		}
		result = upload
		return nil
	})
	return result, finalization, err
}

func (s *AssetService) CompleteAssetUpload(ctx context.Context, projectID, uploadID,
	actorUserID, deviceID, actorSessionID uuid.UUID,
	parts []CompleteMultipartPart) (CompletedAsset, error) {
	upload, _, err := s.loadUploadForWrite(ctx, projectID, uploadID, actorUserID,
		deviceID, actorSessionID, false)
	if err != nil {
		return CompletedAsset{}, err
	}
	if upload.Status == UploadCompleted {
		return s.completedAsset(ctx, upload)
	}
	upload, alreadyVerified, err := s.assembleMultipart(ctx, upload, actorUserID,
		deviceID, actorSessionID, false, parts)
	if err != nil {
		return CompletedAsset{}, err
	}
	if upload.Status == UploadCompleted {
		return s.completedAsset(ctx, upload)
	}
	if !alreadyVerified {
		err = s.verifyAndPromote(ctx, upload)
	}
	if err != nil {
		if accessErr := s.activeActorProjectAccess(ctx, projectID, actorUserID,
			deviceID, actorSessionID); accessErr != nil {
			return CompletedAsset{}, accessErr
		}
		if completed, found := s.assetCompletedAfterRace(ctx, upload); found {
			return completed, nil
		}
		return CompletedAsset{}, err
	}

	stagingKey := upload.ObjectKey
	var result CompletedAsset
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.lockActiveActorTx(tx, actorUserID, deviceID, actorSessionID); err != nil {
			return err
		}
		currentView, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		current, err := s.lockUploadTx(tx, projectID, uploadID, actorUserID, deviceID)
		if err != nil {
			return err
		}
		if current.Status == UploadCompleted {
			upload = current
			return nil
		}
		if err := validateOpenUpload(current, s.now()); err != nil {
			return err
		}
		if err := s.authorizeUploadTx(tx, currentView, actorUserID, deviceID,
			actorSessionID, false); err != nil {
			return err
		}
		blob, err := s.ensureReadyBlobTx(tx, current)
		if err != nil {
			return err
		}
		asset, err := s.linkAssetTx(tx, current, blob)
		if err != nil {
			return err
		}
		now := s.now()
		if err := tx.Model(&model.UploadSession{}).Where("id = ? AND status IN ?", current.ID,
			[]string{UploadPending, UploadUploading}).Updates(map[string]any{
			"blob_id": blob.ID, "object_key": blob.ObjectKey, "status": UploadCompleted,
			"completed_at": now,
		}).Error; err != nil {
			return err
		}
		result = CompletedAsset{Asset: asset, Blob: blob}
		upload = current
		upload.BlobID = &blob.ID
		upload.Status = UploadCompleted
		return nil
	})
	if err != nil {
		return CompletedAsset{}, err
	}
	if result.Asset.AssetID == uuid.Nil {
		result, err = s.completedAsset(ctx, upload)
		if err != nil {
			return CompletedAsset{}, err
		}
	}
	// The durable row already references the server-controlled final key. A
	// failed staging cleanup is harmless and remains eligible for object GC.
	_ = s.Objects.Delete(ctx, stagingKey)
	return result, nil
}

func (s *AssetService) CompleteSnapshotUpload(ctx context.Context, projectID, uploadID,
	actorUserID, deviceID, actorSessionID uuid.UUID,
	parts []CompleteMultipartPart) (CompletedSnapshot, error) {
	upload, _, err := s.loadUploadForWrite(ctx, projectID, uploadID, actorUserID,
		deviceID, actorSessionID, true)
	if err != nil {
		return CompletedSnapshot{}, err
	}
	if err := requireSnapshotAssetsReadyTx(s.DB.WithContext(ctx), projectID,
		upload.SnapshotAssetIDs); err != nil {
		return CompletedSnapshot{}, err
	}
	if upload.Status == UploadCompleted {
		return s.completedSnapshot(ctx, upload)
	}
	upload, alreadyVerified, err := s.assembleMultipart(ctx, upload, actorUserID,
		deviceID, actorSessionID, true, parts)
	if err != nil {
		return CompletedSnapshot{}, err
	}
	if upload.Status == UploadCompleted {
		return s.completedSnapshot(ctx, upload)
	}
	if !alreadyVerified {
		err = s.verifyAndPromote(ctx, upload)
	}
	if err != nil {
		if accessErr := s.activeActorProjectAccess(ctx, projectID, actorUserID,
			deviceID, actorSessionID); accessErr != nil {
			return CompletedSnapshot{}, accessErr
		}
		if completed, found := s.snapshotCompletedAfterRace(ctx, upload); found {
			return completed, nil
		}
		return CompletedSnapshot{}, err
	}

	stagingKey := upload.ObjectKey
	var result CompletedSnapshot
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.lockActiveActorTx(tx, actorUserID, deviceID, actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		current, err := s.lockUploadTx(tx, projectID, uploadID, actorUserID, deviceID)
		if err != nil {
			return err
		}
		if err := requireSnapshotAssetsReadyTx(tx, projectID,
			current.SnapshotAssetIDs); err != nil {
			return err
		}
		if current.Status == UploadCompleted {
			upload = current
			return nil
		}
		if err := validateOpenUpload(current, s.now()); err != nil {
			return err
		}
		if err := s.authorizeUploadTx(tx, view, actorUserID, deviceID,
			actorSessionID, true); err != nil {
			return err
		}
		if current.SnapshotSeq == nil || *current.SnapshotSeq != view.Project.HeadSeq {
			return ErrConflict
		}
		request, err := pendingSnapshotForUploadTx(tx, view.Project,
			normalizedUpload{
				ProjectID: view.Project.ID, ActorUserID: actorUserID,
				DeviceID: deviceID, ActorSessionID: actorSessionID,
				Kind: "project_snapshot", SnapshotSeq: current.SnapshotSeq,
			}, true)
		if err != nil {
			return err
		}
		blob, err := s.ensureReadyBlobTx(tx, current)
		if err != nil {
			return err
		}
		snapshot, err := s.linkSnapshotTx(tx, view.Project, current, blob, false)
		if err != nil {
			return err
		}
		now := s.now()
		if err := tx.Model(&model.UploadSession{}).Where("id = ? AND status IN ?", current.ID,
			[]string{UploadPending, UploadUploading}).Updates(map[string]any{
			"blob_id": blob.ID, "object_key": blob.ObjectKey, "status": UploadCompleted,
			"completed_at": now,
		}).Error; err != nil {
			return err
		}
		if err := tx.Model(&model.CloudProject{}).Where("id = ?", projectID).Updates(map[string]any{
			"snapshot_seq": snapshot.Seq, "updated_at": now,
		}).Error; err != nil {
			return err
		}
		if err := retainLatestSnapshotsTx(tx, projectID, 2); err != nil {
			return err
		}
		var finalization *SnapshotFinalization
		if request != nil {
			finalization, err = completeSnapshotRequestTx(tx, *request, snapshot, now)
			if err != nil {
				return err
			}
		}
		result = CompletedSnapshot{
			Snapshot: snapshot, Blob: blob, Finalization: finalization,
		}
		upload = current
		upload.BlobID = &blob.ID
		upload.Status = UploadCompleted
		return nil
	})
	if err != nil {
		return CompletedSnapshot{}, err
	}
	if result.Snapshot.ID == uuid.Nil {
		result, err = s.completedSnapshot(ctx, upload)
		if err != nil {
			return CompletedSnapshot{}, err
		}
	}
	_ = s.Objects.Delete(ctx, stagingKey)
	return result, nil
}

func (s *AssetService) AbortUpload(ctx context.Context, projectID, uploadID,
	actorUserID, deviceID, actorSessionID uuid.UUID) error {
	var cleanup model.UploadSession
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.lockActiveActorTx(tx, actorUserID, deviceID, actorSessionID); err != nil {
			return err
		}
		if _, err := s.projectAccess(tx, projectID, actorUserID, true); err != nil {
			return err
		}
		upload, err := s.lockUploadTx(tx, projectID, uploadID, actorUserID, deviceID)
		if err != nil {
			return err
		}
		if upload.Status == UploadCompleted {
			return ErrUploadState
		}
		cleanup = upload
		updates := map[string]any{"status": UploadAborted}
		if upload.UploadMode == UploadModeMultipart && upload.ProviderUploadID != "" &&
			upload.MultipartState != MultipartStateAborted {
			updates["multipart_state"] = MultipartStateAborting
			cleanup.MultipartState = MultipartStateAborting
		}
		return tx.Model(&model.UploadSession{}).Where("id = ?", uploadID).Updates(updates).Error
	})
	if err != nil {
		return err
	}
	if cleanup.ID == uuid.Nil {
		return nil
	}
	if cleanup.UploadMode == UploadModeMultipart && cleanup.ProviderUploadID != "" {
		if err := s.Objects.AbortMultipart(ctx, cleanup.ObjectKey,
			cleanup.ProviderUploadID); err != nil {
			return err
		}
	}
	if err := s.Objects.Delete(ctx, cleanup.ObjectKey); err != nil {
		return err
	}
	if cleanup.UploadMode == UploadModeMultipart && cleanup.ProviderUploadID != "" {
		if err := s.DB.WithContext(ctx).Model(&model.UploadSession{}).
			Where("id = ? AND status = ?", cleanup.ID, UploadAborted).
			Update("multipart_state", MultipartStateAborted).Error; err != nil {
			return err
		}
	}
	return nil
}

func (s *AssetService) AssetDownload(ctx context.Context, projectID, assetID,
	actorUserID, deviceID, actorSessionID uuid.UUID) (PresignedDownload, error) {
	if projectID == uuid.Nil || assetID == uuid.Nil || actorUserID == uuid.Nil ||
		deviceID == uuid.Nil || actorSessionID == uuid.Nil {
		return PresignedDownload{}, ErrNotFound
	}
	var result PresignedDownload
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.lockActiveActorTx(tx, actorUserID, deviceID, actorSessionID); err != nil {
			return err
		}
		if _, err := s.projectAccess(tx, projectID, actorUserID, true); err != nil {
			return err
		}
		var asset model.ProjectAsset
		if err := tx.Where("project_id = ? AND asset_id = ?", projectID, assetID).
			First(&asset).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		var blob model.Blob
		if err := tx.Where("id = ? AND status = ?", asset.BlobID, BlobReady).
			First(&blob).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrAssetUnavailable
			}
			return err
		}
		var err error
		result, err = s.download(ctx, blob)
		return err
	})
	if err != nil {
		return PresignedDownload{}, err
	}
	return result, nil
}

func (s *AssetService) SnapshotDownload(ctx context.Context, projectID, snapshotID,
	actorUserID, deviceID, actorSessionID uuid.UUID) (PresignedDownload, error) {
	if projectID == uuid.Nil || snapshotID == uuid.Nil || actorUserID == uuid.Nil ||
		deviceID == uuid.Nil || actorSessionID == uuid.Nil {
		return PresignedDownload{}, ErrNotFound
	}
	var result PresignedDownload
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.lockActiveActorTx(tx, actorUserID, deviceID, actorSessionID); err != nil {
			return err
		}
		if _, err := s.projectAccess(tx, projectID, actorUserID, true); err != nil {
			return err
		}
		var snapshot model.ProjectSnapshot
		if err := tx.Where("id = ? AND project_id = ?", snapshotID, projectID).
			First(&snapshot).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		var blob model.Blob
		if err := tx.Where("id = ? AND status = ? AND kind = ?",
			snapshot.BlobID, BlobReady, "project_snapshot").First(&blob).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrAssetUnavailable
			}
			return err
		}
		var err error
		result, err = s.download(ctx, blob)
		return err
	})
	if err != nil {
		return PresignedDownload{}, err
	}
	return result, nil
}

func (s *AssetService) download(ctx context.Context, blob model.Blob) (PresignedDownload, error) {
	_, ttl, _ := s.normalizedDurations()
	request, err := s.Objects.PresignGet(ctx, blob.ObjectKey, ttl)
	if err != nil {
		return PresignedDownload{}, err
	}
	return PresignedDownload{
		Request: request, SHA256: blob.SHA256, Bytes: blob.Bytes, ContentType: blob.ContentType,
	}, nil
}

func (s *AssetService) uploadPreparation(ctx context.Context, upload model.UploadSession,
	partNumberStart int, actor normalizedUpload) (UploadPreparation, error) {
	uploadMode := upload.UploadMode
	if uploadMode == "" {
		uploadMode = UploadModeSingle
	}
	preparation := UploadPreparation{
		UploadID: upload.ID, AssetID: upload.AssetID, SnapshotSeq: upload.SnapshotSeq,
		Status: upload.Status, UploadMode: uploadMode,
		AlreadyAvailable: upload.Status == UploadCompleted,
		ExpiresAt:        upload.ExpiresAt,
	}
	if upload.Status == UploadCompleted {
		return preparation, nil
	}
	if err := validateOpenUpload(upload, s.now()); err != nil {
		return UploadPreparation{}, err
	}
	if uploadMode == UploadModeMultipart {
		return s.multipartPreparation(ctx, upload, partNumberStart, actor, preparation)
	}
	if partNumberStart < 0 || partNumberStart > 1 {
		return UploadPreparation{}, invalidf("partNumberStart is not valid for a single upload")
	}
	return s.finalizeSinglePreparation(ctx, upload, actor, preparation)
}

func (s *AssetService) loadUploadForWrite(ctx context.Context, projectID, uploadID,
	actorUserID, deviceID, actorSessionID uuid.UUID,
	snapshot bool) (model.UploadSession, ProjectView, error) {
	if projectID == uuid.Nil || uploadID == uuid.Nil || actorUserID == uuid.Nil ||
		deviceID == uuid.Nil || actorSessionID == uuid.Nil {
		return model.UploadSession{}, ProjectView{}, ErrNotFound
	}
	var view ProjectView
	var upload model.UploadSession
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.lockActiveActorTx(tx, actorUserID, deviceID, actorSessionID); err != nil {
			return err
		}
		var err error
		view, err = s.projectAccess(tx, projectID, actorUserID, true)
		if err != nil {
			return err
		}
		if err := tx.First(&upload, "id = ? AND project_id = ?", uploadID, projectID).Error; err != nil {
			if errors.Is(err, gorm.ErrRecordNotFound) {
				return ErrNotFound
			}
			return err
		}
		return nil
	})
	if err != nil {
		return model.UploadSession{}, ProjectView{}, err
	}
	if upload.CreatedBy != actorUserID || upload.DeviceID == nil || *upload.DeviceID != deviceID {
		return model.UploadSession{}, ProjectView{}, ErrForbidden
	}
	if (upload.Kind == "project_snapshot") != snapshot {
		return model.UploadSession{}, ProjectView{}, ErrNotFound
	}
	if upload.Status == UploadCompleted {
		return upload, view, nil
	}
	if err := s.authorizeUploadTx(s.DB.WithContext(ctx), view, actorUserID,
		deviceID, actorSessionID, snapshot); err != nil {
		return model.UploadSession{}, ProjectView{}, err
	}
	if err := validateOpenUpload(upload, s.now()); err != nil {
		return model.UploadSession{}, ProjectView{}, err
	}
	return upload, view, nil
}

func (s *AssetService) lockUploadTx(tx *gorm.DB, projectID, uploadID,
	actorUserID, deviceID uuid.UUID) (model.UploadSession, error) {
	var upload model.UploadSession
	if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
		First(&upload, "id = ? AND project_id = ?", uploadID, projectID).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			return model.UploadSession{}, ErrNotFound
		}
		return model.UploadSession{}, err
	}
	if upload.CreatedBy != actorUserID || upload.DeviceID == nil || *upload.DeviceID != deviceID {
		return model.UploadSession{}, ErrForbidden
	}
	return upload, nil
}

func (s *AssetService) authorizeUploadTx(tx *gorm.DB, view ProjectView,
	actorUserID, deviceID, actorSessionID uuid.UUID, snapshot bool) error {
	if !RoleAllows(view.Role, PermissionEdit) {
		return ErrForbidden
	}
	switch view.Project.Status {
	case model.ProjectUploading:
		if view.Role != model.ProjectRoleOwner {
			return ErrForbidden
		}
		return nil
	case model.ProjectActive, model.ProjectConflict:
		if !snapshot {
			if view.Project.Status != model.ProjectActive {
				return ErrProjectInactive
			}
			return nil
		}
		var count int64
		err := tx.Table("project_live_sessions AS sessions").
			Joins("JOIN project_session_members AS members ON members.id = sessions.host_member_id").
			Where(`sessions.project_id = ? AND sessions.status IN ?
				AND members.user_id = ? AND members.device_id = ?
				AND members.desktop_session_id = ? AND members.left_at IS NULL`,
				view.Project.ID, []string{model.ProjectSessionActive,
					model.ProjectSessionEnding}, actorUserID, deviceID,
				actorSessionID).
			Count(&count).Error
		if err != nil {
			return err
		}
		if count != 1 {
			return ErrForbidden
		}
		return nil
	default:
		return ErrProjectInactive
	}
}

func (s *AssetService) projectAccess(tx *gorm.DB, projectID,
	userID uuid.UUID, lock bool) (ProjectView, error) {
	store := &Store{DB: tx, Now: s.Now}
	return store.projectAccess(tx, projectID, userID, lock)
}

func (s *AssetService) lockActiveActorTx(tx *gorm.DB, actorUserID, deviceID,
	actorSessionID uuid.UUID) error {
	store := &Store{DB: tx, Now: s.Now}
	return store.requireActiveActorTx(tx, actorUserID, deviceID, actorSessionID)
}

func (s *AssetService) activeActorProjectAccess(ctx context.Context, projectID,
	actorUserID, deviceID, actorSessionID uuid.UUID) error {
	return s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.lockActiveActorTx(tx, actorUserID, deviceID, actorSessionID); err != nil {
			return err
		}
		_, err := s.projectAccess(tx, projectID, actorUserID, true)
		return err
	})
}

func (s *AssetService) verifyAndPromote(ctx context.Context, upload model.UploadSession) error {
	expected := objectstore.ExpectedObject{
		SHA256: upload.ExpectedSHA256, Bytes: upload.ExpectedBytes, ContentType: upload.ContentType,
	}
	return s.Objects.VerifyAndPromote(ctx, upload.ObjectKey,
		finalObjectKey(upload.ExpectedSHA256), expected)
}

func (s *AssetService) findDedupeBlobTx(tx *gorm.DB,
	upload normalizedUpload) (model.Blob, bool, error) {
	var blob model.Blob
	// A hash alone is not proof that a caller possesses private project data.
	// Instant (bandwidth-saving) dedupe is therefore limited to blobs already
	// referenced by this authorized project. Cross-project blobs are still
	// storage-deduplicated by ensureReadyBlobTx, but only after the server has
	// independently streamed and verified this upload's exact bytes.
	lookup := tx.Table("blobs").Select("blobs.*")
	if upload.Kind == "project_snapshot" {
		lookup = lookup.Joins("JOIN project_snapshots AS refs ON refs.blob_id = blobs.id").
			Where("blobs.sha256 = ? AND refs.project_id = ?", upload.SHA256, upload.ProjectID)
	} else {
		lookup = lookup.Joins("JOIN project_assets AS refs ON refs.blob_id = blobs.id").
			Where("blobs.sha256 = ? AND refs.project_id = ?", upload.SHA256, upload.ProjectID)
	}
	lookup = lookup.First(&blob)
	if errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
		return model.Blob{}, false, nil
	}
	if lookup.Error != nil {
		return model.Blob{}, false, lookup.Error
	}
	if blob.Bytes != upload.Bytes {
		return model.Blob{}, false, ErrConflict
	}
	if blob.Status != BlobReady {
		return model.Blob{}, false, nil
	}
	if upload.Kind == "project_snapshot" && blob.Kind != "project_snapshot" {
		return model.Blob{}, false, ErrConflict
	}
	if upload.Kind != "project_snapshot" && blob.Kind == "project_snapshot" {
		return model.Blob{}, false, ErrConflict
	}
	return blob, true, nil
}

func (s *AssetService) ensureReadyBlobTx(tx *gorm.DB,
	upload model.UploadSession) (model.Blob, error) {
	now := s.now()
	createdBy := upload.CreatedBy
	candidate := model.Blob{
		ID: uuid.New(), SHA256: upload.ExpectedSHA256, Bytes: upload.ExpectedBytes,
		ContentType: upload.ContentType, Kind: upload.Kind,
		ObjectKey: finalObjectKey(upload.ExpectedSHA256), Status: BlobReady,
		CreatedBy: &createdBy, CreatedAt: now, VerifiedAt: &now,
	}
	if err := tx.Clauses(clause.OnConflict{
		Columns: []clause.Column{{Name: "sha256"}}, DoNothing: true,
	}).Create(&candidate).Error; err != nil {
		return model.Blob{}, err
	}
	var blob model.Blob
	if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).
		Where("sha256 = ?", upload.ExpectedSHA256).First(&blob).Error; err != nil {
		return model.Blob{}, err
	}
	if blob.Bytes != upload.ExpectedBytes || blob.Status != BlobReady ||
		blob.ObjectKey != finalObjectKey(upload.ExpectedSHA256) ||
		((upload.Kind == "project_snapshot") != (blob.Kind == "project_snapshot")) {
		return model.Blob{}, ErrConflict
	}
	return blob, nil
}

func (s *AssetService) linkAssetTx(tx *gorm.DB, upload model.UploadSession,
	blob model.Blob) (model.ProjectAsset, error) {
	if upload.AssetID == nil || *upload.AssetID == uuid.Nil || upload.Kind == "project_snapshot" {
		return model.ProjectAsset{}, ErrValidation
	}
	if err := retainReadyBlobTx(tx, blob.ID); err != nil {
		return model.ProjectAsset{}, err
	}
	var existing model.ProjectAsset
	lookup := tx.Where("project_id = ? AND asset_id = ?", upload.ProjectID, *upload.AssetID).
		First(&existing)
	if lookup.Error == nil {
		if existing.BlobID != blob.ID || existing.Kind != upload.Kind {
			return model.ProjectAsset{}, ErrConflict
		}
		return existing, nil
	}
	if !errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
		return model.ProjectAsset{}, lookup.Error
	}
	createdBy := upload.CreatedBy
	asset := model.ProjectAsset{
		ProjectID: upload.ProjectID, AssetID: *upload.AssetID, BlobID: blob.ID,
		Kind: upload.Kind, DisplayName: upload.DisplayName, CreatedBy: &createdBy,
		CreatedAt: s.now(),
	}
	if err := tx.Create(&asset).Error; err != nil {
		return model.ProjectAsset{}, err
	}
	return asset, nil
}

func (s *AssetService) linkSnapshotTx(tx *gorm.DB, project model.CloudProject,
	upload model.UploadSession, blob model.Blob, preparing bool) (model.ProjectSnapshot, error) {
	if upload.SnapshotSeq == nil || upload.SnapshotSchemaVersion == nil ||
		upload.Kind != "project_snapshot" {
		return model.ProjectSnapshot{}, ErrValidation
	}
	if *upload.SnapshotSeq != project.HeadSeq {
		return model.ProjectSnapshot{}, ErrConflict
	}
	if err := retainReadyBlobTx(tx, blob.ID); err != nil {
		return model.ProjectSnapshot{}, err
	}
	var existing model.ProjectSnapshot
	lookup := tx.Where("project_id = ? AND seq = ?", project.ID, *upload.SnapshotSeq).
		First(&existing)
	if lookup.Error == nil {
		if existing.BlobID != blob.ID || existing.SchemaVersion != *upload.SnapshotSchemaVersion ||
			!canonicalJSONEqual(existing.AssetIDs, upload.SnapshotAssetIDs) {
			return model.ProjectSnapshot{}, ErrConflict
		}
		return existing, nil
	}
	if !errors.Is(lookup.Error, gorm.ErrRecordNotFound) {
		return model.ProjectSnapshot{}, lookup.Error
	}
	createdBy := upload.CreatedBy
	item := model.ProjectSnapshot{
		ID: uuid.New(), ProjectID: project.ID, Seq: *upload.SnapshotSeq,
		BlobID: blob.ID, SchemaVersion: *upload.SnapshotSchemaVersion,
		AssetIDs:  storedSnapshotAssetManifest(upload.SnapshotAssetIDs),
		CreatedBy: &createdBy, CreatedAt: s.now(),
	}
	if err := tx.Create(&item).Error; err != nil {
		return model.ProjectSnapshot{}, err
	}
	if preparing {
		if err := tx.Model(&model.CloudProject{}).Where("id = ?", project.ID).
			Updates(map[string]any{"snapshot_seq": item.Seq, "updated_at": s.now()}).Error; err != nil {
			return model.ProjectSnapshot{}, err
		}
	}
	return item, nil
}

// retainReadyBlobTx serializes reference creation with delayed garbage
// collection. A blob already claimed by the collector cannot acquire a new
// project reference after its provider object may have been deleted.
func retainReadyBlobTx(tx *gorm.DB, blobID uuid.UUID) error {
	result := tx.Model(&model.Blob{}).
		Where("id = ? AND status = ?", blobID, BlobReady).
		Update("unreferenced_at", nil)
	if result.Error != nil {
		return result.Error
	}
	if result.RowsAffected != 1 {
		return ErrAssetUnavailable
	}
	return nil
}

func (s *AssetService) completedAsset(ctx context.Context,
	upload model.UploadSession) (CompletedAsset, error) {
	if upload.BlobID == nil || upload.AssetID == nil {
		return CompletedAsset{}, ErrConflict
	}
	var asset model.ProjectAsset
	if err := s.DB.WithContext(ctx).First(&asset, "project_id = ? AND asset_id = ?",
		upload.ProjectID, *upload.AssetID).Error; err != nil {
		return CompletedAsset{}, err
	}
	var blob model.Blob
	if err := s.DB.WithContext(ctx).First(&blob, "id = ? AND status = ?",
		*upload.BlobID, BlobReady).Error; err != nil {
		return CompletedAsset{}, err
	}
	return CompletedAsset{Asset: asset, Blob: blob}, nil
}

func (s *AssetService) completedSnapshot(ctx context.Context,
	upload model.UploadSession) (CompletedSnapshot, error) {
	if upload.BlobID == nil || upload.SnapshotSeq == nil {
		return CompletedSnapshot{}, ErrConflict
	}
	var snapshot model.ProjectSnapshot
	if err := s.DB.WithContext(ctx).First(&snapshot, "project_id = ? AND seq = ?",
		upload.ProjectID, *upload.SnapshotSeq).Error; err != nil {
		return CompletedSnapshot{}, err
	}
	var blob model.Blob
	if err := s.DB.WithContext(ctx).First(&blob, "id = ? AND status = ? AND kind = ?",
		*upload.BlobID, BlobReady, "project_snapshot").Error; err != nil {
		return CompletedSnapshot{}, err
	}
	return CompletedSnapshot{Snapshot: snapshot, Blob: blob}, nil
}

func (s *AssetService) assetCompletedAfterRace(ctx context.Context,
	upload model.UploadSession) (CompletedAsset, bool) {
	var current model.UploadSession
	if err := s.DB.WithContext(ctx).First(&current, "id = ? AND project_id = ? AND status = ?",
		upload.ID, upload.ProjectID, UploadCompleted).Error; err != nil {
		return CompletedAsset{}, false
	}
	completed, err := s.completedAsset(ctx, current)
	return completed, err == nil
}

func (s *AssetService) snapshotCompletedAfterRace(ctx context.Context,
	upload model.UploadSession) (CompletedSnapshot, bool) {
	var current model.UploadSession
	if err := s.DB.WithContext(ctx).First(&current, "id = ? AND project_id = ? AND status = ?",
		upload.ID, upload.ProjectID, UploadCompleted).Error; err != nil {
		return CompletedSnapshot{}, false
	}
	completed, err := s.completedSnapshot(ctx, current)
	return completed, err == nil
}

func (s *AssetService) normalizeAssetUpload(input PrepareAssetUploadInput) (normalizedUpload, error) {
	if input.ProjectID == uuid.Nil || input.UploadID == uuid.Nil || input.AssetID == uuid.Nil ||
		input.ActorUserID == uuid.Nil || input.DeviceID == uuid.Nil || input.ActorSessionID == uuid.Nil ||
		input.PartNumberStart < 0 || input.PartNumberStart > objectstore.MaximumMultipartParts {
		return normalizedUpload{}, invalidf("project, upload, asset, actor and device identifiers are required")
	}
	kind := strings.TrimSpace(input.Kind)
	switch kind {
	case "audio", "sample", "plugin_state", "other":
	default:
		return normalizedUpload{}, invalidf("asset kind is unsupported")
	}
	displayName, err := safeDisplayName(input.DisplayName)
	if err != nil {
		return normalizedUpload{}, err
	}
	assetID := input.AssetID
	normalized := normalizedUpload{
		ProjectID: input.ProjectID, UploadID: input.UploadID, AssetID: &assetID,
		ActorUserID: input.ActorUserID, DeviceID: input.DeviceID,
		ActorSessionID: input.ActorSessionID,
		SHA256:         strings.TrimSpace(input.SHA256), Bytes: input.Bytes, Kind: kind,
		ContentType: strings.TrimSpace(input.ContentType), DisplayName: displayName,
	}
	if err := s.validateNormalized(normalized); err != nil {
		return normalizedUpload{}, err
	}
	normalized.RequestHash = uploadRequestHash(normalized)
	return normalized, nil
}

func (s *AssetService) normalizeSnapshotUpload(input PrepareSnapshotUploadInput) (normalizedUpload, error) {
	if input.ProjectID == uuid.Nil || input.UploadID == uuid.Nil || input.ActorUserID == uuid.Nil ||
		input.DeviceID == uuid.Nil || input.ActorSessionID == uuid.Nil || input.Seq < 0 ||
		input.SchemaVersion != CollaborationProjectFormatVersion || input.PartNumberStart < 0 ||
		input.PartNumberStart > objectstore.MaximumMultipartParts {
		return normalizedUpload{}, invalidf("snapshot identifiers, sequence and schema version are invalid")
	}
	assetIDs, err := normalizeSnapshotAssetManifest(input.AssetIDs)
	if err != nil {
		return normalizedUpload{}, err
	}
	seq, schemaVersion := input.Seq, input.SchemaVersion
	normalized := normalizedUpload{
		ProjectID: input.ProjectID, UploadID: input.UploadID,
		SnapshotSeq: &seq, SnapshotSchemaVersion: &schemaVersion,
		ActorUserID: input.ActorUserID, DeviceID: input.DeviceID,
		ActorSessionID: input.ActorSessionID,
		SHA256:         strings.TrimSpace(input.SHA256), Bytes: input.Bytes,
		Kind: "project_snapshot", ContentType: strings.TrimSpace(input.ContentType),
		SnapshotAssetIDs: assetIDs,
	}
	if err := s.validateNormalized(normalized); err != nil {
		return normalizedUpload{}, err
	}
	normalized.RequestHash = uploadRequestHash(normalized)
	return normalized, nil
}

func (s *AssetService) validateNormalized(upload normalizedUpload) error {
	if !lowercaseSHA256(upload.SHA256) || upload.Bytes <= 0 ||
		(s.MaximumBytes > 0 && upload.Bytes > s.MaximumBytes) {
		return invalidf("upload checksum or byte size is invalid")
	}
	if !utf8.ValidString(upload.ContentType) || upload.ContentType == "" ||
		len(upload.ContentType) > 160 || strings.ContainsAny(upload.ContentType, "\r\n") {
		return invalidf("upload content type is invalid")
	}
	return nil
}

func validateOpenUpload(upload model.UploadSession, now time.Time) error {
	switch upload.Status {
	case UploadPending, UploadUploading:
		if !now.Before(upload.ExpiresAt) {
			return ErrUploadExpired
		}
		return nil
	case UploadExpired:
		return ErrUploadExpired
	default:
		return ErrUploadState
	}
}

func safeDisplayName(value string) (string, error) {
	value = strings.TrimSpace(strings.ReplaceAll(value, "\\", "/"))
	value = path.Base(value)
	if value == "" || value == "." || value == "/" || !utf8.ValidString(value) ||
		len([]rune(value)) > 255 || strings.ContainsAny(value, "\x00\r\n") {
		return "", invalidf("asset display name is invalid")
	}
	return value, nil
}

func lowercaseSHA256(value string) bool {
	if len(value) != sha256.Size*2 || strings.ToLower(value) != value {
		return false
	}
	decoded, err := hex.DecodeString(value)
	return err == nil && len(decoded) == sha256.Size
}

func normalizeSnapshotAssetManifest(values []string) (datatypes.JSON, error) {
	if len(values) > MaximumSnapshotAssets {
		return nil, invalidf("snapshot asset manifest exceeds %d identifiers",
			MaximumSnapshotAssets)
	}
	canonical := make([]string, len(values))
	seen := make(map[string]struct{}, len(values))
	for index, value := range values {
		parsed, err := uuid.Parse(value)
		if err != nil || parsed == uuid.Nil || parsed.String() != value {
			return nil, invalidf("snapshot asset identifiers must be canonical UUIDs")
		}
		if _, exists := seen[value]; exists {
			return nil, invalidf("snapshot asset identifiers must be unique")
		}
		seen[value] = struct{}{}
		canonical[index] = value
	}
	sort.Strings(canonical)
	encoded, err := json.Marshal(canonical)
	if err != nil {
		return nil, err
	}
	return datatypes.JSON(encoded), nil
}

func storedSnapshotAssetManifest(value datatypes.JSON) datatypes.JSON {
	if len(value) == 0 {
		return datatypes.JSON([]byte("[]"))
	}
	return value
}

func decodeSnapshotAssetManifest(value datatypes.JSON) ([]string, error) {
	value = storedSnapshotAssetManifest(value)
	var identifiers []string
	if err := json.Unmarshal(value, &identifiers); err != nil || identifiers == nil ||
		len(identifiers) > MaximumSnapshotAssets {
		return nil, ErrConflict
	}
	previous := ""
	for _, identifier := range identifiers {
		parsed, err := uuid.Parse(identifier)
		if err != nil || parsed == uuid.Nil || parsed.String() != identifier ||
			(previous != "" && previous >= identifier) {
			return nil, ErrConflict
		}
		previous = identifier
	}
	return identifiers, nil
}

func canonicalJSONEqual(left, right datatypes.JSON) bool {
	leftIDs, leftErr := decodeSnapshotAssetManifest(left)
	rightIDs, rightErr := decodeSnapshotAssetManifest(right)
	if leftErr != nil || rightErr != nil || len(leftIDs) != len(rightIDs) {
		return false
	}
	for index := range leftIDs {
		if leftIDs[index] != rightIDs[index] {
			return false
		}
	}
	return true
}

func snapshotUploadManifestMatches(existing model.UploadSession,
	normalized normalizedUpload) bool {
	if normalized.Kind != "project_snapshot" {
		identifiers, err := decodeSnapshotAssetManifest(existing.SnapshotAssetIDs)
		return err == nil && len(identifiers) == 0
	}
	return canonicalJSONEqual(existing.SnapshotAssetIDs, normalized.SnapshotAssetIDs)
}

func uploadRequestMatches(existing model.UploadSession, normalized normalizedUpload) bool {
	if existing.RequestHash == normalized.RequestHash {
		return true
	}
	// Migration 000012 gives pre-contract snapshot uploads an empty manifest.
	// Preserve resumability only for an explicitly empty retry; a non-empty
	// manifest can never inherit the legacy hash that did not bind asset IDs.
	if normalized.Kind != "project_snapshot" {
		return false
	}
	existingIDs, existingErr := decodeSnapshotAssetManifest(existing.SnapshotAssetIDs)
	normalizedIDs, normalizedErr := decodeSnapshotAssetManifest(normalized.SnapshotAssetIDs)
	return existingErr == nil && normalizedErr == nil && len(existingIDs) == 0 &&
		len(normalizedIDs) == 0 && existing.RequestHash == legacyUploadRequestHash(normalized)
}

func requireSnapshotAssetsReadyTx(tx *gorm.DB, projectID uuid.UUID,
	manifest datatypes.JSON) error {
	identifiers, err := decodeSnapshotAssetManifest(manifest)
	if err != nil {
		return err
	}
	if len(identifiers) == 0 {
		return nil
	}
	var ready int64
	if err := tx.Raw(`SELECT count(*)
		FROM jsonb_array_elements_text(CAST(? AS jsonb)) AS requested(asset_id)
		JOIN project_assets AS assets
		  ON assets.project_id = ? AND assets.asset_id = requested.asset_id::uuid
		JOIN blobs ON blobs.id = assets.blob_id
		WHERE blobs.status = ? AND blobs.kind = assets.kind`,
		string(storedSnapshotAssetManifest(manifest)), projectID, BlobReady).
		Scan(&ready).Error; err != nil {
		return err
	}
	if ready != int64(len(identifiers)) {
		return ErrAssetUnavailable
	}
	return nil
}

func uploadRequestHash(upload normalizedUpload) string {
	return uploadRequestHashWithManifest(upload, true)
}

func legacyUploadRequestHash(upload normalizedUpload) string {
	return uploadRequestHashWithManifest(upload, false)
}

func uploadRequestHashWithManifest(upload normalizedUpload, includeManifest bool) string {
	var manifest json.RawMessage
	if includeManifest && upload.Kind == "project_snapshot" {
		manifest = json.RawMessage(storedSnapshotAssetManifest(upload.SnapshotAssetIDs))
	}
	value, _ := json.Marshal(struct {
		ProjectID             uuid.UUID       `json:"projectId"`
		UploadID              uuid.UUID       `json:"uploadId"`
		AssetID               *uuid.UUID      `json:"assetId"`
		SnapshotSeq           *int64          `json:"snapshotSeq"`
		SnapshotSchemaVersion *int            `json:"snapshotSchemaVersion"`
		SnapshotAssetIDs      json.RawMessage `json:"assetIds,omitempty"`
		SHA256                string          `json:"sha256"`
		Bytes                 int64           `json:"byteSize"`
		Kind                  string          `json:"kind"`
		ContentType           string          `json:"contentType"`
		DisplayName           string          `json:"displayName"`
	}{
		upload.ProjectID, upload.UploadID, upload.AssetID, upload.SnapshotSeq,
		upload.SnapshotSchemaVersion, manifest, upload.SHA256, upload.Bytes, upload.Kind,
		upload.ContentType, upload.DisplayName,
	})
	digest := sha256.Sum256(value)
	return fmt.Sprintf("%x", digest[:])
}

func stagingObjectKey(projectID, uploadID uuid.UUID) string {
	return "uploads/" + projectID.String() + "/" + uploadID.String()
}

func finalObjectKey(sha256Value string) string {
	return "blobs/" + sha256Value[:2] + "/" + sha256Value
}
