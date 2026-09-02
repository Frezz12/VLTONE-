package collab

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/objectstore"
)

func (s *AssetService) ensureMultipartCreated(ctx context.Context, upload model.UploadSession,
	actor normalizedUpload) (model.UploadSession, error) {
	if upload.Status == UploadCompleted || upload.UploadMode != UploadModeMultipart {
		return upload, nil
	}
	var candidate model.UploadSession
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		current, err := s.authorizedMultipartCreationTx(tx, actor)
		candidate = current
		return err
	})
	if err != nil {
		return model.UploadSession{}, err
	}
	if candidate.Status == UploadCompleted || candidate.UploadMode != UploadModeMultipart {
		return candidate, nil
	}
	if candidate.MultipartState != MultipartStateCreating {
		if candidate.ProviderUploadID == "" {
			return model.UploadSession{}, ErrUploadState
		}
		return candidate, nil
	}

	// Provider I/O deliberately happens without any database row lock. A
	// concurrent prepare may create another provider upload; the second
	// transaction below chooses one with a row-locked compare-and-set and the
	// loser aborts its opaque provider upload outside the transaction.
	createdProviderID, err := s.Objects.CreateMultipart(ctx, candidate.ObjectKey,
		objectstore.ExpectedObject{
			SHA256: candidate.ExpectedSHA256, Bytes: candidate.ExpectedBytes,
			ContentType: candidate.ContentType,
		})
	if err != nil {
		return model.UploadSession{}, err
	}
	persisted := false
	var result model.UploadSession
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		current, err := s.authorizedMultipartCreationTx(tx, actor)
		if err != nil {
			return err
		}
		result = current
		if current.Status == UploadCompleted || current.UploadMode != UploadModeMultipart ||
			current.MultipartState != MultipartStateCreating {
			return nil
		}
		if err := tx.Model(&model.UploadSession{}).Where("id = ?", current.ID).
			Updates(map[string]any{
				"provider_upload_id": createdProviderID,
				"multipart_state":    MultipartStateOpen,
			}).Error; err != nil {
			return err
		}
		current.ProviderUploadID = createdProviderID
		current.MultipartState = MultipartStateOpen
		result = current
		persisted = true
		return nil
	})
	if err != nil || !persisted {
		if cleanupErr := s.queueUnclaimedMultipartCleanup(candidate.ObjectKey,
			createdProviderID); cleanupErr != nil {
			err = errors.Join(err, cleanupErr)
		}
	}
	return result, err
}

func (s *AssetService) authorizedMultipartCreationTx(tx *gorm.DB,
	actor normalizedUpload) (model.UploadSession, error) {
	if err := s.lockActiveActorTx(tx, actor.ActorUserID,
		actor.DeviceID, actor.ActorSessionID); err != nil {
		return model.UploadSession{}, err
	}
	view, err := s.projectAccess(tx, actor.ProjectID, actor.ActorUserID, true)
	if err != nil {
		return model.UploadSession{}, err
	}
	current, err := s.lockUploadTx(tx, actor.ProjectID, actor.UploadID,
		actor.ActorUserID, actor.DeviceID)
	if err != nil {
		return model.UploadSession{}, err
	}
	if current.RequestHash != actor.RequestHash {
		return model.UploadSession{}, ErrConflict
	}
	if current.Status == UploadCompleted {
		return current, nil
	}
	if err := validateOpenUpload(current, s.now()); err != nil {
		return model.UploadSession{}, err
	}
	if err := s.authorizeUploadTx(tx, view, actor.ActorUserID, actor.DeviceID,
		actor.ActorSessionID, actor.Kind == "project_snapshot"); err != nil {
		return model.UploadSession{}, err
	}
	if actor.Kind == "project_snapshot" &&
		(current.SnapshotSeq == nil || *current.SnapshotSeq != view.Project.HeadSeq) {
		return model.UploadSession{}, ErrConflict
	}
	return current, nil
}

func (s *AssetService) queueUnclaimedMultipartCleanup(objectKey,
	providerUploadID string) error {
	if providerUploadID == "" {
		return nil
	}
	cleanupContext, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	var cleanupID uuid.UUID
	err := s.DB.WithContext(cleanupContext).Transaction(func(tx *gorm.DB) error {
		var enqueueErr error
		cleanupID, enqueueErr = enqueueObjectCleanupTx(tx, objectKey,
			providerUploadID, true, false, s.now())
		return enqueueErr
	})
	if err == nil {
		// A provider failure is already captured by the durable job and must not
		// fail the successful prepare that won the database race.
		if cleanupErr := s.attemptObjectCleanup(cleanupContext, cleanupID); cleanupErr != nil {
		}
		return nil
	}
	// If the database itself is unavailable, make one best-effort synchronous
	// abort and surface both failures only when the provider also cannot clean up.
	abortErr := s.Objects.AbortMultipart(cleanupContext, objectKey, providerUploadID)
	if abortErr == nil || errors.Is(abortErr, objectstore.ErrMultipartNotFound) {
		return nil
	}
	return errors.Join(err, abortErr)
}

func (s *AssetService) multipartPreparation(ctx context.Context, upload model.UploadSession,
	partNumberStart int, actor normalizedUpload,
	preparation UploadPreparation) (UploadPreparation, error) {
	if upload.MultipartPartSize == nil || upload.MultipartPartCount == nil ||
		*upload.MultipartPartCount < 2 || upload.ProviderUploadID == "" {
		return UploadPreparation{}, ErrUploadState
	}
	partSize, partCount := *upload.MultipartPartSize, *upload.MultipartPartCount
	preparation.MultipartPartSize = &partSize
	preparation.MultipartPartCount = &partCount
	if partNumberStart == 0 {
		partNumberStart = 1
	}
	if partNumberStart < 1 || partNumberStart > partCount {
		return UploadPreparation{}, invalidf("partNumberStart is outside the multipart layout")
	}

	var observed []objectstore.UploadedPart
	switch upload.MultipartState {
	case MultipartStateOpen:
		parts, err := s.Objects.ListMultipartParts(ctx, upload.ObjectKey,
			upload.ProviderUploadID, objectstore.MaximumMultipartParts)
		if err != nil {
			if errors.Is(err, objectstore.ErrMultipartNotFound) {
				return UploadPreparation{}, ErrUploadState
			}
			return UploadPreparation{}, err
		}
		if err := validateObservedMultipartParts(upload, parts, false); err != nil {
			return UploadPreparation{}, err
		}
		observed = parts
	case MultipartStateCompleting, MultipartStateAssembled:
		parts, err := storedMultipartManifest(upload)
		if err != nil {
			return UploadPreparation{}, err
		}
		if err := validateObservedMultipartParts(upload, parts, true); err != nil {
			return UploadPreparation{}, err
		}
		observed = parts
	default:
		return UploadPreparation{}, ErrUploadState
	}

	return s.finalizeMultipartPreparation(ctx, upload, actor, observed,
		partNumberStart, preparation)
}

func (s *AssetService) finalizeSinglePreparation(ctx context.Context,
	upload model.UploadSession, actor normalizedUpload,
	preparation UploadPreparation) (UploadPreparation, error) {
	var result UploadPreparation
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		current, err := s.authorizedMultipartCreationTx(tx, actor)
		if err != nil {
			return err
		}
		mode := current.UploadMode
		if mode == "" {
			mode = UploadModeSingle
		}
		result = preparation
		result.Status = current.Status
		result.UploadMode = mode
		result.ExpiresAt = current.ExpiresAt
		result.AlreadyAvailable = current.Status == UploadCompleted
		if current.Status == UploadCompleted {
			result.Request = nil
			return nil
		}
		if mode != UploadModeSingle {
			return ErrConflict
		}
		uploadTTL, err := s.uploadPresignTTL(current.ExpiresAt)
		if err != nil {
			return err
		}
		// PresignPut is a local, side-effect-free SigV4 calculation. Keeping it
		// inside this short authorization transaction closes the revoke/member
		// removal issuance race without performing provider network I/O.
		request, err := s.Objects.PresignPut(ctx, current.ObjectKey,
			objectstore.ExpectedObject{
				SHA256: current.ExpectedSHA256, Bytes: current.ExpectedBytes,
				ContentType: current.ContentType,
			}, uploadTTL)
		if err != nil {
			return err
		}
		result.Request = &request
		return nil
	})
	return result, err
}

func (s *AssetService) finalizeMultipartPreparation(ctx context.Context,
	upload model.UploadSession, actor normalizedUpload, observed []objectstore.UploadedPart,
	partNumberStart int, preparation UploadPreparation) (UploadPreparation, error) {
	var result UploadPreparation
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		current, err := s.authorizedMultipartCreationTx(tx, actor)
		if err != nil {
			return err
		}
		result = preparation
		result.Status = current.Status
		result.UploadMode = current.UploadMode
		result.ExpiresAt = current.ExpiresAt
		result.AlreadyAvailable = current.Status == UploadCompleted
		result.Parts = nil
		result.NextPartNumberStart = nil
		if current.Status == UploadCompleted {
			result.UploadedParts = nil
			return nil
		}
		if current.UploadMode != UploadModeMultipart || current.MultipartPartSize == nil ||
			current.MultipartPartCount == nil || current.ProviderUploadID == "" ||
			current.ProviderUploadID != upload.ProviderUploadID {
			return ErrConflict
		}
		partSize, partCount := *current.MultipartPartSize, *current.MultipartPartCount
		result.MultipartPartSize = &partSize
		result.MultipartPartCount = &partCount
		if partNumberStart < 1 || partNumberStart > partCount {
			return invalidf("partNumberStart is outside the multipart layout")
		}
		if current.MultipartState != MultipartStateOpen {
			if current.MultipartState != MultipartStateCompleting &&
				current.MultipartState != MultipartStateAssembled {
				return ErrUploadState
			}
			stored, err := storedMultipartManifest(current)
			if err != nil || validateObservedMultipartParts(current, stored, true) != nil {
				return ErrConflict
			}
			result.UploadedParts = publicMultipartParts(stored)
			return nil
		}
		if err := validateObservedMultipartParts(current, observed, false); err != nil {
			return err
		}
		manifest, err := marshalMultipartManifest(observed)
		if err != nil {
			return err
		}
		if err := tx.Model(&model.UploadSession{}).Where("id = ?", current.ID).
			Update("multipart_manifest", manifest).Error; err != nil {
			return err
		}
		result.UploadedParts = publicMultipartParts(observed)
		uploaded := make(map[int]struct{}, len(observed))
		for _, part := range observed {
			uploaded[part.PartNumber] = struct{}{}
		}
		_, _, batch := s.normalizedMultipartConfig()
		partNumbers, next := missingMultipartPartPage(partCount, partNumberStart, batch, uploaded)
		result.NextPartNumberStart = next
		uploadTTL, err := s.uploadPresignTTL(current.ExpiresAt)
		if err != nil {
			return err
		}
		for _, partNumber := range partNumbers {
			bytes, err := multipartPartBytes(current.ExpectedBytes, partSize,
				partCount, partNumber)
			if err != nil {
				return err
			}
			// Like PresignPut, this is local HMAC only; no provider request is
			// performed while the actor/project/upload locks are held.
			request, err := s.Objects.PresignMultipartPart(ctx, current.ObjectKey,
				current.ProviderUploadID, partNumber, bytes, uploadTTL)
			if err != nil {
				return err
			}
			result.Parts = append(result.Parts, PreparedMultipartPart{
				PartNumber: partNumber, ByteSize: bytes, Request: request,
			})
		}
		return nil
	})
	return result, err
}

func (s *AssetService) uploadPresignTTL(expiresAt time.Time) (time.Duration, error) {
	uploadTTL, _, _ := s.normalizedDurations()
	// SigV4 expiry is encoded as whole seconds. Flooring guarantees a delegated
	// URL can never outlive the durable upload session, even when PostgreSQL and
	// the application clock have different sub-second precision.
	remaining := expiresAt.Sub(s.now()).Truncate(time.Second)
	if remaining < time.Second {
		return 0, ErrUploadExpired
	}
	uploadTTL = uploadTTL.Truncate(time.Second)
	if uploadTTL < time.Second {
		return 0, ErrUploadExpired
	}
	if remaining < uploadTTL {
		uploadTTL = remaining
	}
	return uploadTTL, nil
}

func (s *AssetService) assembleMultipart(ctx context.Context, upload model.UploadSession,
	actorUserID, deviceID, actorSessionID uuid.UUID, snapshot bool,
	submitted []CompleteMultipartPart) (model.UploadSession, bool, error) {
	if upload.UploadMode != UploadModeMultipart {
		if len(submitted) != 0 {
			return model.UploadSession{}, false, invalidf("single upload completion cannot contain multipart parts")
		}
		return upload, false, nil
	}
	if upload.MultipartPartSize == nil || upload.MultipartPartCount == nil ||
		upload.ProviderUploadID == "" {
		return model.UploadSession{}, false, ErrUploadState
	}
	if upload.MultipartState == MultipartStateAssembled {
		if len(submitted) != 0 {
			stored, err := storedMultipartManifest(upload)
			if err != nil || !submittedManifestMatches(submitted, stored) {
				return model.UploadSession{}, false, ErrConflict
			}
		}
		return upload, false, nil
	}

	var manifest []objectstore.UploadedPart
	switch upload.MultipartState {
	case MultipartStateOpen:
		observed, err := s.Objects.ListMultipartParts(ctx, upload.ObjectKey,
			upload.ProviderUploadID, objectstore.MaximumMultipartParts)
		if err != nil {
			return model.UploadSession{}, false, err
		}
		if err := validateObservedMultipartParts(upload, observed, true); err != nil {
			return model.UploadSession{}, false, err
		}
		if !submittedManifestMatches(submitted, observed) {
			return model.UploadSession{}, false, ErrConflict
		}
		manifest = observed
		current, err := s.markMultipartCompleting(ctx, upload, actorUserID, deviceID,
			actorSessionID, snapshot, manifest)
		if err != nil {
			return model.UploadSession{}, false, err
		}
		upload = current
		if upload.Status == UploadCompleted || upload.MultipartState == MultipartStateAssembled {
			return upload, false, nil
		}
	case MultipartStateCompleting:
		stored, err := storedMultipartManifest(upload)
		if err != nil || !submittedManifestMatches(submitted, stored) {
			return model.UploadSession{}, false, ErrConflict
		}
		manifest = stored
		observed, listErr := s.Objects.ListMultipartParts(ctx, upload.ObjectKey,
			upload.ProviderUploadID, objectstore.MaximumMultipartParts)
		if listErr == nil {
			if err := validateObservedMultipartParts(upload, observed, true); err != nil ||
				!multipartManifestsEqual(stored, observed) {
				return model.UploadSession{}, false, ErrConflict
			}
			manifest = observed
		} else if !errors.Is(listErr, objectstore.ErrMultipartNotFound) {
			return model.UploadSession{}, false, listErr
		}
	default:
		return model.UploadSession{}, false, ErrUploadState
	}

	err := s.Objects.CompleteMultipart(ctx, upload.ObjectKey,
		upload.ProviderUploadID, manifest)
	if errors.Is(err, objectstore.ErrMultipartNotFound) {
		// Recovery for a crash after the provider assembled the staging object but
		// before the database state advanced. Only a full independent hash/size
		// verification is accepted as proof that completion actually succeeded.
		// This crash-recovery proof is still a full verification job. Route it
		// through the same global/per-user semaphore as the ordinary completion
		// path; otherwise repeated provider-not-found retries could bypass both
		// V1 verification limits.
		if verifyErr := s.verifyWithLimits(ctx, upload); verifyErr != nil {
			return model.UploadSession{}, false, verifyErr
		}
		current, stateErr := s.markMultipartAssembled(ctx, upload, actorUserID,
			deviceID, actorSessionID, snapshot)
		return current, true, stateErr
	}
	if err != nil {
		return model.UploadSession{}, false, err
	}
	current, err := s.markMultipartAssembled(ctx, upload, actorUserID,
		deviceID, actorSessionID, snapshot)
	return current, false, err
}

func (s *AssetService) markMultipartCompleting(ctx context.Context, upload model.UploadSession,
	actorUserID, deviceID, actorSessionID uuid.UUID, snapshot bool,
	parts []objectstore.UploadedPart) (model.UploadSession, error) {
	manifest, err := marshalMultipartManifest(parts)
	if err != nil {
		return model.UploadSession{}, err
	}
	return s.transitionMultipartState(ctx, upload, actorUserID, deviceID, actorSessionID,
		snapshot, MultipartStateCompleting, manifest)
}

func (s *AssetService) markMultipartAssembled(ctx context.Context, upload model.UploadSession,
	actorUserID, deviceID, actorSessionID uuid.UUID,
	snapshot bool) (model.UploadSession, error) {
	return s.transitionMultipartState(ctx, upload, actorUserID, deviceID, actorSessionID,
		snapshot, MultipartStateAssembled, nil)
}

func (s *AssetService) transitionMultipartState(ctx context.Context, upload model.UploadSession,
	actorUserID, deviceID, actorSessionID uuid.UUID, snapshot bool,
	target string, manifest datatypes.JSON) (model.UploadSession, error) {
	var result model.UploadSession
	err := s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.lockActiveActorTx(tx, actorUserID, deviceID, actorSessionID); err != nil {
			return err
		}
		view, err := s.projectAccess(tx, upload.ProjectID, actorUserID, true)
		if err != nil {
			return err
		}
		current, err := s.lockUploadTx(tx, upload.ProjectID, upload.ID,
			actorUserID, deviceID)
		if err != nil {
			return err
		}
		if current.Status == UploadCompleted {
			result = current
			return nil
		}
		if err := validateOpenUpload(current, s.now()); err != nil {
			return err
		}
		if err := s.authorizeUploadTx(tx, view, actorUserID, deviceID,
			actorSessionID, snapshot); err != nil {
			return err
		}
		if snapshot && (current.SnapshotSeq == nil ||
			*current.SnapshotSeq != view.Project.HeadSeq) {
			return ErrConflict
		}
		if current.UploadMode != UploadModeMultipart ||
			current.ProviderUploadID != upload.ProviderUploadID {
			return ErrConflict
		}
		switch target {
		case MultipartStateCompleting:
			if current.MultipartState == MultipartStateAssembled {
				result = current
				return nil
			}
			if current.MultipartState == MultipartStateCompleting {
				stored, decodeErr := storedMultipartManifest(current)
				candidate, candidateErr := decodeMultipartManifest(manifest)
				if decodeErr != nil || candidateErr != nil ||
					!multipartManifestsEqual(stored, candidate) {
					return ErrConflict
				}
				result = current
				return nil
			}
			if current.MultipartState != MultipartStateOpen {
				return ErrUploadState
			}
			if err := tx.Model(&model.UploadSession{}).Where("id = ?", current.ID).
				Updates(map[string]any{
					"multipart_state":    MultipartStateCompleting,
					"multipart_manifest": manifest,
				}).Error; err != nil {
				return err
			}
			current.MultipartState = MultipartStateCompleting
			current.MultipartManifest = manifest
		case MultipartStateAssembled:
			if current.MultipartState == MultipartStateAssembled {
				result = current
				return nil
			}
			if current.MultipartState != MultipartStateCompleting {
				return ErrUploadState
			}
			if err := tx.Model(&model.UploadSession{}).Where("id = ?", current.ID).
				Update("multipart_state", MultipartStateAssembled).Error; err != nil {
				return err
			}
			current.MultipartState = MultipartStateAssembled
		default:
			return ErrUploadState
		}
		result = current
		return nil
	})
	return result, err
}

func validateObservedMultipartParts(upload model.UploadSession,
	parts []objectstore.UploadedPart, requireComplete bool) error {
	if upload.MultipartPartSize == nil || upload.MultipartPartCount == nil ||
		len(parts) > *upload.MultipartPartCount ||
		(requireComplete && len(parts) != *upload.MultipartPartCount) {
		return ErrConflict
	}
	previous := 0
	for _, part := range parts {
		if part.PartNumber <= previous || part.PartNumber > *upload.MultipartPartCount ||
			!validMultipartETag(part.ETag) {
			return ErrConflict
		}
		expectedBytes, err := multipartPartBytes(upload.ExpectedBytes,
			*upload.MultipartPartSize, *upload.MultipartPartCount, part.PartNumber)
		if err != nil || expectedBytes != part.Bytes {
			return ErrConflict
		}
		previous = part.PartNumber
	}
	if requireComplete {
		for index, part := range parts {
			if part.PartNumber != index+1 {
				return ErrConflict
			}
		}
	}
	return nil
}

func submittedManifestMatches(submitted []CompleteMultipartPart,
	provider []objectstore.UploadedPart) bool {
	if len(submitted) != len(provider) {
		return false
	}
	for index, part := range submitted {
		if part.PartNumber != index+1 || !validMultipartETag(part.ETag) ||
			provider[index].PartNumber != part.PartNumber || provider[index].ETag != part.ETag {
			return false
		}
	}
	return true
}

func multipartManifestsEqual(left, right []objectstore.UploadedPart) bool {
	if len(left) != len(right) {
		return false
	}
	for index := range left {
		if left[index] != right[index] {
			return false
		}
	}
	return true
}

func marshalMultipartManifest(parts []objectstore.UploadedPart) (datatypes.JSON, error) {
	encoded, err := json.Marshal(publicMultipartParts(parts))
	if err != nil {
		return nil, ErrConflict
	}
	return datatypes.JSON(encoded), nil
}

func storedMultipartManifest(upload model.UploadSession) ([]objectstore.UploadedPart, error) {
	return decodeMultipartManifest(upload.MultipartManifest)
}

func decodeMultipartManifest(raw []byte) ([]objectstore.UploadedPart, error) {
	if len(raw) == 0 {
		raw = []byte("[]")
	}
	var stored []UploadedMultipartPart
	if err := json.Unmarshal(raw, &stored); err != nil ||
		len(stored) > objectstore.MaximumMultipartParts {
		return nil, ErrConflict
	}
	parts := make([]objectstore.UploadedPart, 0, len(stored))
	for _, part := range stored {
		if part.PartNumber < 1 || !validMultipartETag(part.ETag) || part.ByteSize <= 0 {
			return nil, ErrConflict
		}
		parts = append(parts, objectstore.UploadedPart{
			PartNumber: part.PartNumber, ETag: part.ETag, Bytes: part.ByteSize,
		})
	}
	return parts, nil
}

func publicMultipartParts(parts []objectstore.UploadedPart) []UploadedMultipartPart {
	if len(parts) == 0 {
		return nil
	}
	result := make([]UploadedMultipartPart, 0, len(parts))
	for _, part := range parts {
		result = append(result, UploadedMultipartPart{
			PartNumber: part.PartNumber, ByteSize: part.Bytes, ETag: part.ETag,
		})
	}
	return result
}

func validMultipartETag(value string) bool {
	if value == "" || len(value) > 256 || !utf8.ValidString(value) ||
		strings.TrimSpace(value) != value {
		return false
	}
	for _, character := range value {
		if character < 0x20 || character == 0x7f {
			return false
		}
	}
	return true
}
