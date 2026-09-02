package api

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"

	"github.com/google/uuid"

	"vltstudio/backend/internal/collab"
	"vltstudio/backend/internal/objectstore"
)

const maxSnapshotPrepareJSONBody = 5 << 20

type prepareAssetUploadRequest struct {
	UploadID        uuid.UUID `json:"uploadId"`
	AssetID         uuid.UUID `json:"assetId"`
	SHA256          string    `json:"sha256"`
	ByteSize        int64     `json:"byteSize"`
	Kind            string    `json:"kind"`
	ContentType     string    `json:"contentType"`
	DisplayName     string    `json:"displayName"`
	PartNumberStart int       `json:"partNumberStart"`
}

type prepareSnapshotUploadRequest struct {
	UploadID        uuid.UUID `json:"uploadId"`
	Seq             int64     `json:"seq"`
	SchemaVersion   int       `json:"schemaVersion"`
	SHA256          string    `json:"sha256"`
	ByteSize        int64     `json:"byteSize"`
	ContentType     string    `json:"contentType"`
	AssetIDs        *[]string `json:"assetIds"`
	PartNumberStart int       `json:"partNumberStart"`
}

type completeUploadRequest struct {
	Parts []collab.CompleteMultipartPart `json:"parts"`
}

func (s *Server) prepareProjectAssetUpload(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	var input prepareAssetUploadRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	preparation, err := s.CollabAssets.PrepareAssetUpload(r.Context(), collab.PrepareAssetUploadInput{
		ProjectID: projectID, UploadID: input.UploadID, AssetID: input.AssetID,
		ActorUserID: userFrom(r).ID, DeviceID: deviceFrom(r).ID,
		ActorSessionID: collaborationActorSessionID(r),
		SHA256:         input.SHA256, Bytes: input.ByteSize, Kind: input.Kind,
		ContentType: input.ContentType, DisplayName: input.DisplayName,
		PartNumberStart: input.PartNumberStart,
	})
	if err != nil {
		s.writeCollaborationStorageError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, preparation)
}

func (s *Server) completeProjectAssetUpload(w http.ResponseWriter, r *http.Request) {
	projectID, uploadID, ok := collaborationUploadIDs(w, r)
	if !ok {
		return
	}
	input, ok := decodeOptionalCompleteUpload(w, r)
	if !ok {
		return
	}
	completed, err := s.CollabAssets.CompleteAssetUpload(r.Context(), projectID, uploadID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r), input.Parts)
	if err != nil {
		s.writeCollaborationStorageError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, completed)
}

func (s *Server) abortProjectUpload(w http.ResponseWriter, r *http.Request) {
	projectID, uploadID, ok := collaborationUploadIDs(w, r)
	if !ok {
		return
	}
	if err := s.CollabAssets.AbortUpload(r.Context(), projectID, uploadID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r)); err != nil {
		s.writeCollaborationStorageError(w, r, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) prepareProjectSnapshotUpload(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	var input prepareSnapshotUploadRequest
	if !decodeJSONWithLimit(w, r, &input, maxSnapshotPrepareJSONBody) {
		return
	}
	if input.AssetIDs == nil {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed",
			"Snapshot asset manifest is required.", map[string]string{
				"assetIds": "Provide an array; use an empty array when the snapshot has no assets.",
			})
		return
	}
	preparation, err := s.CollabAssets.PrepareSnapshotUpload(r.Context(), collab.PrepareSnapshotUploadInput{
		ProjectID: projectID, UploadID: input.UploadID, ActorUserID: userFrom(r).ID,
		DeviceID: deviceFrom(r).ID, ActorSessionID: collaborationActorSessionID(r),
		Seq: input.Seq, SchemaVersion: input.SchemaVersion,
		SHA256: input.SHA256, Bytes: input.ByteSize, ContentType: input.ContentType,
		AssetIDs:        *input.AssetIDs,
		PartNumberStart: input.PartNumberStart,
	})
	if err != nil {
		s.writeCollaborationStorageError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, preparation)
	s.publishSnapshotFinalization(preparation.Finalization)
}

func (s *Server) completeProjectSnapshotUpload(w http.ResponseWriter, r *http.Request) {
	projectID, uploadID, ok := collaborationUploadIDs(w, r)
	if !ok {
		return
	}
	input, ok := decodeOptionalCompleteUpload(w, r)
	if !ok {
		return
	}
	completed, err := s.CollabAssets.CompleteSnapshotUpload(r.Context(), projectID, uploadID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r), input.Parts)
	if err != nil {
		s.writeCollaborationStorageError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, completed)
	s.publishSnapshotFinalization(completed.Finalization)
}

func (s *Server) projectAssetDownload(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	assetID, ok := parseUUIDParam(w, r, "assetID")
	if !ok {
		return
	}
	download, err := s.CollabAssets.AssetDownload(r.Context(), projectID, assetID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r))
	if err != nil {
		s.writeCollaborationStorageError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, download)
}

func (s *Server) projectSnapshotDownload(w http.ResponseWriter, r *http.Request) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return
	}
	snapshotID, ok := parseUUIDParam(w, r, "snapshotID")
	if !ok {
		return
	}
	download, err := s.CollabAssets.SnapshotDownload(r.Context(), projectID, snapshotID,
		userFrom(r).ID, deviceFrom(r).ID, collaborationActorSessionID(r))
	if err != nil {
		s.writeCollaborationStorageError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, download)
}

func collaborationUploadIDs(w http.ResponseWriter, r *http.Request) (uuid.UUID, uuid.UUID, bool) {
	projectID, ok := parseUUIDParam(w, r, "projectID")
	if !ok {
		return uuid.Nil, uuid.Nil, false
	}
	uploadID, ok := parseUUIDParam(w, r, "uploadID")
	return projectID, uploadID, ok
}

func decodeOptionalCompleteUpload(w http.ResponseWriter, r *http.Request) (completeUploadRequest, bool) {
	var input completeUploadRequest
	if r.Body == nil || r.ContentLength == 0 {
		return input, true
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxJSONBody)
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&input); err != nil {
		if errors.Is(err, io.EOF) {
			return completeUploadRequest{}, true
		}
		writeError(w, r, http.StatusBadRequest, "invalid_request", "Invalid request body.", nil)
		return completeUploadRequest{}, false
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		writeError(w, r, http.StatusBadRequest, "invalid_request", "Only one JSON value is allowed.", nil)
		return completeUploadRequest{}, false
	}
	if len(input.Parts) > objectstore.MaximumMultipartParts {
		writeError(w, r, http.StatusUnprocessableEntity, "validation_failed",
			"Multipart completion contains too many parts.", nil)
		return completeUploadRequest{}, false
	}
	return input, true
}

func (s *Server) writeCollaborationStorageError(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, objectstore.ErrInvalidObject), errors.Is(err, objectstore.ErrObjectNotFound):
		writeError(w, r, http.StatusUnprocessableEntity, "upload_verification_failed",
			"Uploaded bytes do not match the declared checksum and size.", nil)
	case errors.Is(err, objectstore.ErrProvider):
		writeError(w, r, http.StatusServiceUnavailable, "object_storage_unavailable",
			"Cloud storage is temporarily unavailable.", nil)
	case errors.Is(err, objectstore.ErrMultipartNotFound):
		writeError(w, r, http.StatusConflict, "multipart_upload_unavailable",
			"The provider multipart upload is no longer available. Abort it and prepare a new upload.", nil)
	case errors.Is(err, objectstore.ErrInvalidMultipart):
		writeError(w, r, http.StatusBadGateway, "object_storage_invalid_state",
			"Cloud storage returned invalid multipart state.", nil)
	case errors.Is(err, collab.ErrUploadExpired):
		writeError(w, r, http.StatusGone, "upload_expired",
			"The upload session has expired. Prepare a new upload.", nil)
	case errors.Is(err, collab.ErrUploadState):
		writeError(w, r, http.StatusConflict, "upload_state_conflict",
			"The upload is no longer open.", nil)
	case errors.Is(err, collab.ErrAssetUnavailable):
		writeError(w, r, http.StatusConflict, "asset_incomplete",
			"The requested asset is not verified and ready.", nil)
	default:
		s.writeCollaborationError(w, r, err)
	}
}
