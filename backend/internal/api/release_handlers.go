package api

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"io"
	"mime"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
)

const (
	maxReleaseArtifact = int64(2 << 30)
	maxReleaseImage    = int64(10 << 20)
	maxReleaseShots    = int64(10)
)

var (
	errLastReleaseArtifact = errors.New("published release requires an artifact")
	errReleaseValidation   = errors.New("release validation failed")
	errReleaseVersion      = errors.New("release version already exists")
	errScreenshotCaptions  = errors.New("published screenshots require captions")
	errScreenshotLimit     = errors.New("release screenshot limit reached")
)

var artifactKinds = map[string]struct {
	platform string
	label    string
	exts     []string
	mime     string
}{
	"windows-exe":    {"windows", "Windows Setup", []string{".exe"}, "application/vnd.microsoft.portable-executable"},
	"macos-dmg":      {"macos", "macOS DMG", []string{".dmg"}, "application/x-apple-diskimage"},
	"linux-appimage": {"linux", "Linux AppImage", []string{".appimage"}, "application/octet-stream"},
	"linux-deb":      {"linux", "Linux DEB", []string{".deb"}, "application/vnd.debian.binary-package"},
	"linux-rpm":      {"linux", "Linux RPM", []string{".rpm"}, "application/x-rpm"},
	"linux-tar-gz":   {"linux", "Linux TAR.GZ", []string{".tar.gz"}, "application/gzip"},
	"linux-tar-xz":   {"linux", "Linux TAR.XZ", []string{".tar.xz"}, "application/x-xz"},
}

type releaseInput struct {
	Version    string   `json:"version"`
	SummaryRU  string   `json:"summary_ru"`
	SummaryEN  string   `json:"summary_en"`
	FeaturesRU []string `json:"features_ru"`
	FeaturesEN []string `json:"features_en"`
	ChangesRU  []string `json:"changes_ru"`
	ChangesEN  []string `json:"changes_en"`
	FixesRU    []string `json:"fixes_ru"`
	FixesEN    []string `json:"fixes_en"`
}

type releaseArtifactView struct {
	ID          uuid.UUID `json:"id"`
	Kind        string    `json:"kind"`
	Platform    string    `json:"platform"`
	Label       string    `json:"label"`
	FileName    string    `json:"file_name"`
	Bytes       int64     `json:"bytes"`
	SHA256      string    `json:"sha256"`
	DownloadURL string    `json:"download_url"`
	UpdatedAt   time.Time `json:"updated_at"`
}

type releaseScreenshotView struct {
	ID        uuid.UUID `json:"id"`
	Caption   string    `json:"caption,omitempty"`
	CaptionRU string    `json:"caption_ru,omitempty"`
	CaptionEN string    `json:"caption_en,omitempty"`
	SortOrder int       `json:"sort_order"`
	Width     int       `json:"width"`
	Height    int       `json:"height"`
	SHA256    string    `json:"sha256"`
	URL       string    `json:"url"`
}

type releaseView struct {
	ID          uuid.UUID               `json:"id"`
	Version     string                  `json:"version"`
	Status      string                  `json:"status,omitempty"`
	Summary     string                  `json:"summary"`
	SummaryRU   string                  `json:"summary_ru,omitempty"`
	SummaryEN   string                  `json:"summary_en,omitempty"`
	Features    []string                `json:"features"`
	FeaturesRU  []string                `json:"features_ru,omitempty"`
	FeaturesEN  []string                `json:"features_en,omitempty"`
	Changes     []string                `json:"changes"`
	ChangesRU   []string                `json:"changes_ru,omitempty"`
	ChangesEN   []string                `json:"changes_en,omitempty"`
	Fixes       []string                `json:"fixes"`
	FixesRU     []string                `json:"fixes_ru,omitempty"`
	FixesEN     []string                `json:"fixes_en,omitempty"`
	Artifacts   []releaseArtifactView   `json:"artifacts"`
	Screenshots []releaseScreenshotView `json:"screenshots"`
	PageURL     string                  `json:"page_url,omitempty"`
	PublishedAt *time.Time              `json:"published_at"`
	CreatedAt   time.Time               `json:"created_at,omitempty"`
	UpdatedAt   time.Time               `json:"updated_at,omitempty"`
}

func parseReleaseVersion(raw string) ([3]int, bool) {
	var result [3]int
	parts := strings.Split(strings.TrimSpace(raw), ".")
	if len(parts) != 3 {
		return result, false
	}
	for index, part := range parts {
		if part == "" || (len(part) > 1 && part[0] == '0') {
			return result, false
		}
		for _, character := range part {
			if character < '0' || character > '9' {
				return result, false
			}
		}
		value, err := strconv.ParseInt(part, 10, 32)
		if err != nil {
			return result, false
		}
		result[index] = int(value)
	}
	return result, true
}

func normalizedReleaseList(values []string) ([]string, bool) {
	if len(values) > 100 {
		return nil, false
	}
	result := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value == "" {
			continue
		}
		if len([]rune(value)) > 1000 {
			return nil, false
		}
		result = append(result, value)
	}
	return result, true
}

func releaseJSONList(raw datatypes.JSON) []string {
	result := []string{}
	_ = json.Unmarshal(raw, &result)
	return result
}

func (s *Server) releaseFromInput(item *model.Release, input releaseInput) map[string]string {
	fields := map[string]string{}
	version := strings.TrimSpace(input.Version)
	if item.Status == model.ReleasePublished && (item.Version == nil || version != *item.Version) {
		fields["version"] = "Published version numbers cannot be changed."
	} else if version == "" {
		item.Version, item.VersionMajor, item.VersionMinor, item.VersionPatch = nil, nil, nil, nil
	} else if parsed, ok := parseReleaseVersion(version); !ok {
		fields["version"] = "Version must use X.Y.Z without leading zeroes."
	} else {
		item.Version = &version
		item.VersionMajor, item.VersionMinor, item.VersionPatch = &parsed[0], &parsed[1], &parsed[2]
	}
	item.SummaryRU = strings.TrimSpace(input.SummaryRU)
	item.SummaryEN = strings.TrimSpace(input.SummaryEN)
	if len([]rune(item.SummaryRU)) > 4000 {
		fields["summary_ru"] = "Russian summary must be no longer than 4000 characters."
	}
	if len([]rune(item.SummaryEN)) > 4000 {
		fields["summary_en"] = "English summary must be no longer than 4000 characters."
	}
	lists := []struct {
		name   string
		values []string
		target *datatypes.JSON
	}{
		{"features_ru", input.FeaturesRU, &item.FeaturesRU}, {"features_en", input.FeaturesEN, &item.FeaturesEN},
		{"changes_ru", input.ChangesRU, &item.ChangesRU}, {"changes_en", input.ChangesEN, &item.ChangesEN},
		{"fixes_ru", input.FixesRU, &item.FixesRU}, {"fixes_en", input.FixesEN, &item.FixesEN},
	}
	for _, list := range lists {
		values, ok := normalizedReleaseList(list.values)
		if !ok {
			fields[list.name] = "Use no more than 100 items and 1000 characters per item."
			continue
		}
		*list.target = datatypes.JSON(jsonBytes(values))
	}
	return fields
}

func (s *Server) releaseVersionAvailable(id uuid.UUID, version *string) bool {
	return releaseVersionAvailable(s.DB, id, version)
}

func releaseVersionAvailable(db *gorm.DB, id uuid.UUID, version *string) bool {
	if version == nil {
		return true
	}
	var count int64
	return db.Model(&model.Release{}).Where("version = ? AND id <> ?", *version, id).Count(&count).Error == nil && count == 0
}

func (s *Server) adminCreateRelease(w http.ResponseWriter, r *http.Request) {
	var input releaseInput
	if !decodeJSON(w, r, &input) {
		return
	}
	adminID := adminFrom(r).ID
	item := model.Release{ID: uuid.New(), Status: model.ReleaseDraft, CreatedBy: &adminID, UpdatedBy: &adminID}
	if fields := s.releaseFromInput(&item, input); len(fields) != 0 {
		writeError(w, r, 422, "validation_failed", "Release draft contains invalid fields.", fields)
		return
	}
	if !s.releaseVersionAvailable(item.ID, item.Version) {
		writeError(w, r, 409, "version_exists", "This version already exists.", map[string]string{"version": "Version must be unique."})
		return
	}
	if err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Create(&item).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "release.create", "release", item.ID, map[string]any{"version": item.Version})
	}); err != nil {
		writeError(w, r, 500, "release_create_failed", "Release draft could not be created.", nil)
		return
	}
	s.writeAdminRelease(w, r, http.StatusCreated, item)
}

func (s *Server) adminUpdateRelease(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "releaseID")
	if !ok {
		return
	}
	var input releaseInput
	if !decodeJSON(w, r, &input) {
		return
	}
	var item model.Release
	fields := map[string]string{}
	adminID := adminFrom(r).ID
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&item, "id = ?", id).Error; err != nil {
			return err
		}
		fields = s.releaseFromInput(&item, input)
		if item.Status == model.ReleasePublished {
			if item.SummaryRU == "" {
				fields["summary_ru"] = "Russian summary is required."
			}
			if item.SummaryEN == "" {
				fields["summary_en"] = "English summary is required."
			}
		}
		if len(fields) != 0 {
			return errReleaseValidation
		}
		if !releaseVersionAvailable(tx, item.ID, item.Version) {
			return errReleaseVersion
		}
		item.UpdatedBy = &adminID
		item.UpdatedAt = time.Now().UTC()
		if err := tx.Save(&item).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "release.update", "release", item.ID, map[string]any{"version": item.Version, "status": item.Status})
	})
	if errors.Is(err, gorm.ErrRecordNotFound) {
		writeError(w, r, 404, "release_not_found", "Release was not found.", nil)
		return
	}
	if errors.Is(err, errReleaseValidation) {
		writeError(w, r, 422, "validation_failed", "Release contains invalid fields.", fields)
		return
	}
	if errors.Is(err, errReleaseVersion) {
		writeError(w, r, 409, "version_exists", "This version already exists.", map[string]string{"version": "Version must be unique."})
		return
	}
	if err != nil {
		writeError(w, r, 500, "release_update_failed", "Release could not be saved.", nil)
		return
	}
	s.writeAdminRelease(w, r, http.StatusOK, item)
}

func (s *Server) adminPublishRelease(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "releaseID")
	if !ok {
		return
	}
	fields := map[string]string{}
	var item model.Release
	adminID := adminFrom(r).ID
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&item, "id = ?", id).Error; err != nil {
			return err
		}
		if item.Status == model.ReleasePublished {
			return nil
		}
		if item.Version == nil {
			fields["version"] = "Version is required before publishing."
		}
		if item.SummaryRU == "" {
			fields["summary_ru"] = "Russian summary is required before publishing."
		}
		if item.SummaryEN == "" {
			fields["summary_en"] = "English summary is required before publishing."
		}
		var artifactCount, uncaptained int64
		if err := tx.Model(&model.ReleaseArtifact{}).Where("release_id = ?", id).Count(&artifactCount).Error; err != nil {
			return err
		}
		if artifactCount == 0 {
			fields["artifacts"] = "Upload at least one installer before publishing."
		}
		if err := tx.Model(&model.ReleaseScreenshot{}).Where("release_id = ? AND (btrim(caption_ru) = '' OR btrim(caption_en) = '')", id).Count(&uncaptained).Error; err != nil {
			return err
		}
		if uncaptained != 0 {
			fields["screenshots"] = "Every screenshot needs Russian and English captions."
		}
		if len(fields) != 0 {
			return errReleaseValidation
		}
		now := time.Now().UTC()
		item.Status, item.PublishedAt, item.UpdatedAt, item.UpdatedBy = model.ReleasePublished, &now, now, &adminID
		if err := tx.Save(&item).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "release.publish", "release", id, map[string]any{"version": item.Version})
	})
	if errors.Is(err, gorm.ErrRecordNotFound) {
		writeError(w, r, 404, "release_not_found", "Release was not found.", nil)
		return
	}
	if errors.Is(err, errReleaseValidation) {
		writeError(w, r, 422, "release_not_ready", "Release is not ready to publish.", fields)
		return
	}
	if err != nil {
		writeError(w, r, 500, "release_publish_failed", "Release could not be published.", nil)
		return
	}
	s.writeAdminRelease(w, r, http.StatusOK, item)
}

func (s *Server) adminDeleteRelease(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "releaseID")
	if !ok {
		return
	}
	var item model.Release
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&item, "id = ?", id).Error; err != nil {
			return err
		}
		if item.Status != model.ReleaseDraft {
			return errReleaseValidation
		}
		if err := tx.Delete(&model.Release{}, "id = ?", id).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "release.delete", "release", id, map[string]any{"version": item.Version})
	})
	if errors.Is(err, gorm.ErrRecordNotFound) {
		writeError(w, r, 404, "release_not_found", "Release was not found.", nil)
		return
	}
	if errors.Is(err, errReleaseValidation) {
		writeError(w, r, 409, "published_release_immutable", "Published releases cannot be deleted.", nil)
		return
	}
	if err != nil {
		writeError(w, r, 500, "release_delete_failed", "Release draft could not be deleted.", nil)
		return
	}
	_ = os.RemoveAll(filepath.Join(s.Config.StorageRoot, "releases", id.String()))
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) adminReleases(w http.ResponseWriter, r *http.Request) {
	var items []model.Release
	if err := s.DB.Order("(status = 'draft') DESC").Order("updated_at DESC").Find(&items).Error; err != nil {
		writeError(w, r, 500, "releases_unavailable", "Releases are unavailable.", nil)
		return
	}
	views := make([]releaseView, 0, len(items))
	for _, item := range items {
		views = append(views, s.adminReleaseView(item))
	}
	writeJSON(w, http.StatusOK, map[string]any{"releases": views})
}

func (s *Server) adminRelease(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "releaseID")
	if !ok {
		return
	}
	var item model.Release
	if s.DB.First(&item, "id = ?", id).Error != nil {
		writeError(w, r, 404, "release_not_found", "Release was not found.", nil)
		return
	}
	s.writeAdminRelease(w, r, http.StatusOK, item)
}

func (s *Server) writeAdminRelease(w http.ResponseWriter, _ *http.Request, status int, item model.Release) {
	writeJSON(w, status, s.adminReleaseView(item))
}

func (s *Server) adminReleaseView(item model.Release) releaseView {
	artifacts, screenshots := s.releaseRelations(item.ID, valueOrEmpty(item.Version))
	for index := range screenshots {
		screenshots[index].URL = fmt.Sprintf("/v1/admin/releases/%s/screenshots/%s/file", item.ID, screenshots[index].ID)
	}
	return releaseView{
		ID: item.ID, Version: valueOrEmpty(item.Version), Status: item.Status,
		SummaryRU: item.SummaryRU, SummaryEN: item.SummaryEN,
		FeaturesRU: releaseJSONList(item.FeaturesRU), FeaturesEN: releaseJSONList(item.FeaturesEN),
		ChangesRU: releaseJSONList(item.ChangesRU), ChangesEN: releaseJSONList(item.ChangesEN),
		FixesRU: releaseJSONList(item.FixesRU), FixesEN: releaseJSONList(item.FixesEN),
		Artifacts: artifacts, Screenshots: screenshots, PublishedAt: item.PublishedAt,
		CreatedAt: item.CreatedAt, UpdatedAt: item.UpdatedAt,
	}
}

func valueOrEmpty(value *string) string {
	if value == nil {
		return ""
	}
	return *value
}

func (s *Server) releaseRelations(releaseID uuid.UUID, version string) ([]releaseArtifactView, []releaseScreenshotView) {
	var artifacts []model.ReleaseArtifact
	var screenshots []model.ReleaseScreenshot
	s.DB.Where("release_id = ?", releaseID).Order("kind").Find(&artifacts)
	s.DB.Where("release_id = ?", releaseID).Order("sort_order, created_at").Find(&screenshots)
	artifactViews := make([]releaseArtifactView, 0, len(artifacts))
	for _, item := range artifacts {
		kind := artifactKinds[item.Kind]
		artifactViews = append(artifactViews, releaseArtifactView{
			ID: item.ID, Kind: item.Kind, Platform: kind.platform, Label: kind.label,
			FileName: item.FileName, Bytes: item.Bytes, SHA256: item.SHA256,
			DownloadURL: fmt.Sprintf("/v1/releases/%s/download/%s", version, item.Kind), UpdatedAt: item.UpdatedAt,
		})
	}
	shotViews := make([]releaseScreenshotView, 0, len(screenshots))
	for _, item := range screenshots {
		shotViews = append(shotViews, releaseScreenshotView{
			ID: item.ID, CaptionRU: item.CaptionRU, CaptionEN: item.CaptionEN, SortOrder: item.SortOrder,
			Width: item.Width, Height: item.Height, SHA256: item.SHA256,
			URL: fmt.Sprintf("/v1/releases/%s/screenshots/%s", version, item.ID),
		})
	}
	return artifactViews, shotViews
}

func releaseLocale(r *http.Request) (string, bool) {
	locale := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("locale")))
	if locale == "" {
		locale = "en"
	}
	return locale, locale == "ru" || locale == "en"
}

func (s *Server) publicReleases(w http.ResponseWriter, r *http.Request) {
	locale, ok := releaseLocale(r)
	if !ok {
		writeError(w, r, 400, "locale_invalid", "Locale must be ru or en.", nil)
		return
	}
	var items []model.Release
	if err := s.DB.Where("status = ?", model.ReleasePublished).
		Order("version_major DESC, version_minor DESC, version_patch DESC").Find(&items).Error; err != nil {
		writeError(w, r, 500, "releases_unavailable", "Releases are unavailable.", nil)
		return
	}
	views := make([]releaseView, 0, len(items))
	for _, item := range items {
		views = append(views, s.publicReleaseView(item, locale))
	}
	writeJSON(w, http.StatusOK, map[string]any{"releases": views})
}

func (s *Server) publicRelease(w http.ResponseWriter, r *http.Request) {
	locale, ok := releaseLocale(r)
	if !ok {
		writeError(w, r, 400, "locale_invalid", "Locale must be ru or en.", nil)
		return
	}
	version := strings.TrimSpace(chi.URLParam(r, "version"))
	if _, valid := parseReleaseVersion(version); !valid {
		writeError(w, r, 404, "release_not_found", "Release was not found.", nil)
		return
	}
	var item model.Release
	if s.DB.First(&item, "version = ? AND status = ?", version, model.ReleasePublished).Error != nil {
		writeError(w, r, 404, "release_not_found", "Release was not found.", nil)
		return
	}
	writeJSON(w, http.StatusOK, s.publicReleaseView(item, locale))
}

func (s *Server) publicReleaseView(item model.Release, locale string) releaseView {
	version := valueOrEmpty(item.Version)
	artifacts, screenshots := s.releaseRelations(item.ID, version)
	result := releaseView{
		ID: item.ID, Version: version, Artifacts: artifacts, Screenshots: screenshots,
		PageURL:     fmt.Sprintf("%s/%s/releases/%s", s.Config.PublicOrigin, locale, version),
		PublishedAt: item.PublishedAt,
	}
	if locale == "ru" {
		result.Summary, result.Features, result.Changes, result.Fixes = item.SummaryRU, releaseJSONList(item.FeaturesRU), releaseJSONList(item.ChangesRU), releaseJSONList(item.FixesRU)
		for index := range result.Screenshots {
			result.Screenshots[index].Caption = result.Screenshots[index].CaptionRU
			result.Screenshots[index].CaptionRU, result.Screenshots[index].CaptionEN = "", ""
		}
	} else {
		result.Summary, result.Features, result.Changes, result.Fixes = item.SummaryEN, releaseJSONList(item.FeaturesEN), releaseJSONList(item.ChangesEN), releaseJSONList(item.FixesEN)
		for index := range result.Screenshots {
			result.Screenshots[index].Caption = result.Screenshots[index].CaptionEN
			result.Screenshots[index].CaptionRU, result.Screenshots[index].CaptionEN = "", ""
		}
	}
	return result
}

func (s *Server) latestRelease(w http.ResponseWriter, r *http.Request) {
	locale, ok := releaseLocale(r)
	if !ok {
		writeError(w, r, 400, "locale_invalid", "Locale must be ru or en.", nil)
		return
	}
	platform := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("platform")))
	var kinds []string
	switch platform {
	case "windows":
		kinds = []string{"windows-exe"}
	case "macos":
		kinds = []string{"macos-dmg"}
	case "linux":
		kinds = []string{"linux-appimage", "linux-deb", "linux-rpm", "linux-tar-gz", "linux-tar-xz"}
	default:
		writeError(w, r, 400, "platform_invalid", "Platform must be windows, macos or linux.", nil)
		return
	}
	var item model.Release
	err := s.DB.Model(&model.Release{}).Distinct("releases.*").
		Joins("JOIN release_artifacts ON release_artifacts.release_id = releases.id").
		Where("releases.status = ? AND release_artifacts.kind IN ?", model.ReleasePublished, kinds).
		Order("version_major DESC, version_minor DESC, version_patch DESC").First(&item).Error
	if errors.Is(err, gorm.ErrRecordNotFound) {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if err != nil {
		writeError(w, r, 500, "releases_unavailable", "Releases are unavailable.", nil)
		return
	}
	version := valueOrEmpty(item.Version)
	writeJSON(w, http.StatusOK, map[string]any{
		"version": version, "published_at": item.PublishedAt,
		"page_url": fmt.Sprintf("%s/%s/releases/%s", s.Config.PublicOrigin, locale, version),
	})
}

func cleanUploadFilename(value string) string {
	return bounded(filepath.Base(strings.ReplaceAll(value, "\\", "/")), 255)
}

func artifactExtension(kind, name string) (string, bool) {
	definition, ok := artifactKinds[kind]
	if !ok {
		return "", false
	}
	lower := strings.ToLower(name)
	for _, extension := range definition.exts {
		if strings.HasSuffix(lower, extension) {
			return extension, true
		}
	}
	return "", false
}

func (s *Server) adminUploadReleaseArtifact(w http.ResponseWriter, r *http.Request) {
	releaseID, ok := parseUUIDParam(w, r, "releaseID")
	if !ok {
		return
	}
	kindName := strings.TrimSpace(chi.URLParam(r, "kind"))
	definition, validKind := artifactKinds[kindName]
	if !validKind {
		writeError(w, r, 404, "artifact_kind_invalid", "Artifact kind is not supported.", nil)
		return
	}
	var release model.Release
	if s.DB.First(&release, "id = ?", releaseID).Error != nil {
		writeError(w, r, 404, "release_not_found", "Release was not found.", nil)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxReleaseArtifact+(2<<20))
	reader, err := r.MultipartReader()
	if err != nil {
		writeError(w, r, 400, "artifact_invalid", "Upload must be multipart/form-data.", nil)
		return
	}
	directory := filepath.Join(s.Config.StorageRoot, "releases", releaseID.String(), "artifacts")
	if err := os.MkdirAll(directory, 0o700); err != nil {
		writeError(w, r, 500, "storage_unavailable", "Release storage is unavailable.", nil)
		return
	}
	var originalName, extension, temporaryPath string
	defer func() {
		if temporaryPath != "" {
			_ = os.Remove(temporaryPath)
		}
	}()
	var bytesWritten int64
	var digest string
	for {
		part, nextErr := reader.NextPart()
		if nextErr == io.EOF {
			break
		}
		if nextErr != nil {
			writeError(w, r, 400, "artifact_invalid", "Artifact upload is invalid.", nil)
			return
		}
		if part.FormName() != "file" || part.FileName() == "" {
			part.Close()
			continue
		}
		if temporaryPath != "" {
			part.Close()
			_ = os.Remove(temporaryPath)
			writeError(w, r, 422, "artifact_invalid", "Upload exactly one artifact.", nil)
			return
		}
		originalName = cleanUploadFilename(part.FileName())
		if extension, ok = artifactExtension(kindName, originalName); !ok {
			part.Close()
			writeError(w, r, 422, "artifact_type_invalid", "File extension does not match the selected artifact kind.", nil)
			return
		}
		temporary, createErr := os.CreateTemp(directory, ".upload-*")
		if createErr != nil {
			part.Close()
			writeError(w, r, 500, "storage_unavailable", "Artifact could not be stored.", nil)
			return
		}
		temporaryPath = temporary.Name()
		_ = temporary.Chmod(0o600)
		hash := sha256.New()
		bytesWritten, err = io.Copy(io.MultiWriter(temporary, hash), io.LimitReader(part, maxReleaseArtifact+1))
		part.Close()
		closeErr := temporary.Close()
		if err != nil || closeErr != nil || bytesWritten <= 0 || bytesWritten > maxReleaseArtifact {
			_ = os.Remove(temporaryPath)
			if bytesWritten > maxReleaseArtifact {
				writeError(w, r, 413, "artifact_too_large", "Artifact must be no larger than 2 GiB.", nil)
			} else {
				writeError(w, r, 422, "artifact_invalid", "Artifact could not be read.", nil)
			}
			return
		}
		digest = hex.EncodeToString(hash.Sum(nil))
	}
	if temporaryPath == "" {
		writeError(w, r, 422, "artifact_missing", "Choose a file to upload.", nil)
		return
	}
	newID := uuid.New()
	finalPath := filepath.Join(directory, newID.String()+extension)
	if err := os.Rename(temporaryPath, finalPath); err != nil {
		_ = os.Remove(temporaryPath)
		writeError(w, r, 500, "storage_unavailable", "Artifact could not be finalized.", nil)
		return
	}
	temporaryPath = ""
	now := time.Now().UTC()
	artifact := model.ReleaseArtifact{
		ID: newID, ReleaseID: releaseID, Kind: kindName, FileName: originalName,
		MimeType: definition.mime, Path: finalPath, Bytes: bytesWritten, SHA256: digest,
		CreatedAt: now, UpdatedAt: now,
	}
	var previous model.ReleaseArtifact
	dbErr := s.DB.Transaction(func(tx *gorm.DB) error {
		var lockedRelease model.Release
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&lockedRelease, "id = ?", releaseID).Error; err != nil {
			return err
		}
		query := tx.Clauses(clause.Locking{Strength: "UPDATE"}).Where("release_id = ? AND kind = ?", releaseID, kindName).First(&previous)
		if query.Error == nil {
			artifact.ID = previous.ID
			artifact.CreatedAt = previous.CreatedAt
			if err := tx.Model(&model.ReleaseArtifact{}).Where("id = ?", previous.ID).Updates(map[string]any{
				"file_name": artifact.FileName, "mime_type": artifact.MimeType,
				"path": artifact.Path, "bytes": artifact.Bytes, "sha256": artifact.SHA256, "updated_at": now,
			}).Error; err != nil {
				return err
			}
		} else if query.Error == gorm.ErrRecordNotFound {
			if err := tx.Create(&artifact).Error; err != nil {
				return err
			}
		} else {
			return query.Error
		}
		action := "release.artifact.upload"
		details := map[string]any{"kind": kindName, "file_name": originalName, "bytes": bytesWritten, "sha256": digest}
		if previous.Path != "" {
			action = "release.artifact.replace"
			details["previous_file_name"], details["previous_sha256"] = previous.FileName, previous.SHA256
		}
		return s.audit(tx, r, action, "release", releaseID, details)
	})
	if dbErr != nil {
		_ = os.Remove(finalPath)
		writeError(w, r, 500, "artifact_save_failed", "Artifact metadata could not be saved.", nil)
		return
	}
	if previous.Path != "" && previous.Path != finalPath {
		s.removeReleaseFile(previous.Path)
	}
	writeJSON(w, http.StatusOK, releaseArtifactView{
		ID: artifact.ID, Kind: kindName, Platform: definition.platform, Label: definition.label,
		FileName: originalName, Bytes: bytesWritten, SHA256: digest,
		DownloadURL: fmt.Sprintf("/v1/releases/%s/download/%s", valueOrEmpty(release.Version), kindName), UpdatedAt: now,
	})
}

func (s *Server) adminDeleteReleaseArtifact(w http.ResponseWriter, r *http.Request) {
	releaseID, ok := parseUUIDParam(w, r, "releaseID")
	if !ok {
		return
	}
	kind := strings.TrimSpace(chi.URLParam(r, "kind"))
	var release model.Release
	var artifact model.ReleaseArtifact
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&release, "id = ?", releaseID).Error; err != nil {
			return err
		}
		if err := tx.First(&artifact, "release_id = ? AND kind = ?", releaseID, kind).Error; err != nil {
			return err
		}
		if release.Status == model.ReleasePublished {
			var count int64
			if err := tx.Model(&model.ReleaseArtifact{}).Where("release_id = ?", releaseID).Count(&count).Error; err != nil {
				return err
			}
			if count <= 1 {
				return errLastReleaseArtifact
			}
		}
		if err := tx.Delete(&model.ReleaseArtifact{}, "id = ?", artifact.ID).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "release.artifact.delete", "release", releaseID, map[string]any{"kind": kind, "sha256": artifact.SHA256})
	})
	if errors.Is(err, gorm.ErrRecordNotFound) {
		writeError(w, r, 404, "artifact_not_found", "Artifact was not found.", nil)
		return
	}
	if errors.Is(err, errLastReleaseArtifact) {
		writeError(w, r, 409, "last_artifact_required", "A published release must keep at least one installer.", nil)
		return
	}
	if err != nil {
		writeError(w, r, 500, "artifact_delete_failed", "Artifact could not be deleted.", nil)
		return
	}
	s.removeReleaseFile(artifact.Path)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) adminUploadReleaseScreenshot(w http.ResponseWriter, r *http.Request) {
	releaseID, ok := parseUUIDParam(w, r, "releaseID")
	if !ok {
		return
	}
	var release model.Release
	if s.DB.First(&release, "id = ?", releaseID).Error != nil {
		writeError(w, r, 404, "release_not_found", "Release was not found.", nil)
		return
	}
	var count int64
	s.DB.Model(&model.ReleaseScreenshot{}).Where("release_id = ?", releaseID).Count(&count)
	if count >= maxReleaseShots {
		writeError(w, r, 422, "too_many_screenshots", "A release can contain no more than 10 screenshots.", nil)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxReleaseImage+(2<<20))
	if err := r.ParseMultipartForm(maxReleaseImage + (2 << 20)); err != nil {
		writeError(w, r, 400, "screenshot_invalid", "Screenshot upload is too large or invalid.", nil)
		return
	}
	defer r.MultipartForm.RemoveAll()
	files := r.MultipartForm.File["file"]
	if len(files) != 1 {
		writeError(w, r, 422, "screenshot_missing", "Choose exactly one screenshot.", nil)
		return
	}
	captionRU := strings.TrimSpace(r.FormValue("caption_ru"))
	captionEN := strings.TrimSpace(r.FormValue("caption_en"))
	if release.Status == model.ReleasePublished && (captionRU == "" || captionEN == "") {
		writeError(w, r, 422, "validation_failed", "Published screenshots require both captions.", map[string]string{
			"caption_ru": "Russian caption is required.", "caption_en": "English caption is required.",
		})
		return
	}
	body, err := multipartFile(files[0], maxReleaseImage)
	if err != nil {
		writeError(w, r, 422, "screenshot_too_large", "Screenshot must be no larger than 10 MB.", nil)
		return
	}
	sanitized, mimeType, extension, err := sanitizeImage(body)
	if err != nil {
		writeError(w, r, 422, "screenshot_invalid", "Screenshot must be a valid JPEG, PNG or WebP image.", nil)
		return
	}
	configuration, _, err := image.DecodeConfig(bytes.NewReader(sanitized))
	if err != nil {
		writeError(w, r, 422, "screenshot_invalid", "Screenshot dimensions could not be read.", nil)
		return
	}
	directory := filepath.Join(s.Config.StorageRoot, "releases", releaseID.String(), "screenshots")
	if err := os.MkdirAll(directory, 0o700); err != nil {
		writeError(w, r, 500, "storage_unavailable", "Screenshot storage is unavailable.", nil)
		return
	}
	id := uuid.New()
	path := filepath.Join(directory, id.String()+extension)
	if err := os.WriteFile(path, sanitized, 0o600); err != nil {
		writeError(w, r, 500, "storage_unavailable", "Screenshot could not be stored.", nil)
		return
	}
	digest := sha256.Sum256(sanitized)
	sortOrder, _ := strconv.Atoi(r.FormValue("sort_order"))
	item := model.ReleaseScreenshot{
		ID: id, ReleaseID: releaseID, CaptionRU: bounded(captionRU, 1000), CaptionEN: bounded(captionEN, 1000),
		SortOrder: sortOrder, MimeType: mimeType, Path: path, Bytes: int64(len(sanitized)),
		Width: configuration.Width, Height: configuration.Height, SHA256: hex.EncodeToString(digest[:]),
	}
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&release, "id = ?", releaseID).Error; err != nil {
			return err
		}
		if release.Status == model.ReleasePublished && (captionRU == "" || captionEN == "") {
			return errScreenshotCaptions
		}
		var currentCount int64
		if err := tx.Model(&model.ReleaseScreenshot{}).Where("release_id = ?", releaseID).Count(&currentCount).Error; err != nil {
			return err
		}
		if currentCount >= maxReleaseShots {
			return errScreenshotLimit
		}
		if item.SortOrder == 0 {
			var maximum int
			if err := tx.Model(&model.ReleaseScreenshot{}).Where("release_id = ?", releaseID).Select("COALESCE(MAX(sort_order), 0)").Scan(&maximum).Error; err != nil {
				return err
			}
			item.SortOrder = maximum + 10
		}
		if err := tx.Create(&item).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "release.screenshot.upload", "release", releaseID, map[string]any{"screenshot_id": id, "sha256": item.SHA256})
	})
	if errors.Is(err, errScreenshotCaptions) {
		_ = os.Remove(path)
		writeError(w, r, 422, "validation_failed", "Published screenshots require both captions.", map[string]string{
			"caption_ru": "Russian caption is required.", "caption_en": "English caption is required.",
		})
		return
	}
	if errors.Is(err, errScreenshotLimit) {
		_ = os.Remove(path)
		writeError(w, r, 422, "too_many_screenshots", "A release can contain no more than 10 screenshots.", nil)
		return
	}
	if err != nil {
		_ = os.Remove(path)
		writeError(w, r, 500, "screenshot_save_failed", "Screenshot metadata could not be saved.", nil)
		return
	}
	writeJSON(w, http.StatusCreated, releaseScreenshotView{
		ID: id, CaptionRU: item.CaptionRU, CaptionEN: item.CaptionEN, SortOrder: item.SortOrder,
		Width: item.Width, Height: item.Height, SHA256: item.SHA256,
		URL: fmt.Sprintf("/v1/releases/%s/screenshots/%s", valueOrEmpty(release.Version), id),
	})
}

type screenshotInput struct {
	CaptionRU string `json:"caption_ru"`
	CaptionEN string `json:"caption_en"`
	SortOrder int    `json:"sort_order"`
}

func (s *Server) adminUpdateReleaseScreenshot(w http.ResponseWriter, r *http.Request) {
	releaseID, ok := parseUUIDParam(w, r, "releaseID")
	if !ok {
		return
	}
	screenshotID, ok := parseUUIDParam(w, r, "screenshotID")
	if !ok {
		return
	}
	var input screenshotInput
	if !decodeJSON(w, r, &input) {
		return
	}
	var release model.Release
	var item model.ReleaseScreenshot
	if s.DB.First(&release, "id = ?", releaseID).Error != nil ||
		s.DB.First(&item, "id = ? AND release_id = ?", screenshotID, releaseID).Error != nil {
		writeError(w, r, 404, "screenshot_not_found", "Screenshot was not found.", nil)
		return
	}
	input.CaptionRU, input.CaptionEN = strings.TrimSpace(input.CaptionRU), strings.TrimSpace(input.CaptionEN)
	if release.Status == model.ReleasePublished && (input.CaptionRU == "" || input.CaptionEN == "") {
		writeError(w, r, 422, "validation_failed", "Published screenshots require both captions.", nil)
		return
	}
	now := time.Now().UTC()
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&release, "id = ?", releaseID).Error; err != nil {
			return err
		}
		if err := tx.First(&item, "id = ? AND release_id = ?", screenshotID, releaseID).Error; err != nil {
			return err
		}
		if release.Status == model.ReleasePublished && (input.CaptionRU == "" || input.CaptionEN == "") {
			return errScreenshotCaptions
		}
		if err := tx.Model(&item).Updates(map[string]any{
			"caption_ru": bounded(input.CaptionRU, 1000), "caption_en": bounded(input.CaptionEN, 1000),
			"sort_order": input.SortOrder, "updated_at": now,
		}).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "release.screenshot.update", "release", releaseID, map[string]any{"screenshot_id": screenshotID})
	})
	if errors.Is(err, errScreenshotCaptions) {
		writeError(w, r, 422, "validation_failed", "Published screenshots require both captions.", map[string]string{
			"caption_ru": "Russian caption is required.", "caption_en": "English caption is required.",
		})
		return
	}
	if errors.Is(err, gorm.ErrRecordNotFound) {
		writeError(w, r, 404, "screenshot_not_found", "Screenshot was not found.", nil)
		return
	}
	if err != nil {
		writeError(w, r, 500, "screenshot_update_failed", "Screenshot could not be updated.", nil)
		return
	}
	item.CaptionRU, item.CaptionEN, item.SortOrder = bounded(input.CaptionRU, 1000), bounded(input.CaptionEN, 1000), input.SortOrder
	writeJSON(w, http.StatusOK, releaseScreenshotView{
		ID: item.ID, CaptionRU: item.CaptionRU, CaptionEN: item.CaptionEN, SortOrder: item.SortOrder,
		Width: item.Width, Height: item.Height, SHA256: item.SHA256,
		URL: fmt.Sprintf("/v1/releases/%s/screenshots/%s", valueOrEmpty(release.Version), item.ID),
	})
}

func (s *Server) adminDeleteReleaseScreenshot(w http.ResponseWriter, r *http.Request) {
	releaseID, ok := parseUUIDParam(w, r, "releaseID")
	if !ok {
		return
	}
	screenshotID, ok := parseUUIDParam(w, r, "screenshotID")
	if !ok {
		return
	}
	var item model.ReleaseScreenshot
	if s.DB.First(&item, "id = ? AND release_id = ?", screenshotID, releaseID).Error != nil {
		writeError(w, r, 404, "screenshot_not_found", "Screenshot was not found.", nil)
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		var release model.Release
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&release, "id = ?", releaseID).Error; err != nil {
			return err
		}
		if err := tx.First(&item, "id = ? AND release_id = ?", screenshotID, releaseID).Error; err != nil {
			return err
		}
		if err := tx.Delete(&item).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "release.screenshot.delete", "release", releaseID, map[string]any{"screenshot_id": screenshotID})
	})
	if errors.Is(err, gorm.ErrRecordNotFound) {
		writeError(w, r, 404, "screenshot_not_found", "Screenshot was not found.", nil)
		return
	}
	if err != nil {
		writeError(w, r, 500, "screenshot_delete_failed", "Screenshot could not be deleted.", nil)
		return
	}
	s.removeReleaseFile(item.Path)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) adminReleaseScreenshotFile(w http.ResponseWriter, r *http.Request) {
	releaseID, ok := parseUUIDParam(w, r, "releaseID")
	if !ok {
		return
	}
	screenshotID, ok := parseUUIDParam(w, r, "screenshotID")
	if !ok {
		return
	}
	var item model.ReleaseScreenshot
	if s.DB.First(&item, "id = ? AND release_id = ?", screenshotID, releaseID).Error != nil {
		writeError(w, r, 404, "screenshot_not_found", "Screenshot was not found.", nil)
		return
	}
	extension := releaseImageExtension(item.MimeType)
	s.serveReleaseFile(w, r, item.Path, "vlt-release-"+screenshotID.String()+extension, item.MimeType, item.SHA256, true)
}

func (s *Server) releaseFileAllowed(path string) bool {
	root, err := filepath.Abs(filepath.Join(s.Config.StorageRoot, "releases"))
	if err != nil {
		return false
	}
	absolute, err := filepath.Abs(path)
	if err != nil {
		return false
	}
	relative, err := filepath.Rel(root, absolute)
	return err == nil && relative != ".." && !strings.HasPrefix(relative, ".."+string(filepath.Separator))
}

func releaseImageExtension(mimeType string) string {
	switch mimeType {
	case "image/png":
		return ".png"
	case "image/webp":
		return ".webp"
	default:
		return ".jpg"
	}
}

func (s *Server) removeReleaseFile(path string) {
	if path != "" && s.releaseFileAllowed(path) {
		_ = os.Remove(path)
	}
}

func (s *Server) serveReleaseFile(w http.ResponseWriter, r *http.Request, path, name, contentType, sha string, inline bool) {
	if !s.releaseFileAllowed(path) {
		writeError(w, r, 404, "file_not_found", "File was not found.", nil)
		return
	}
	file, err := os.Open(path)
	if err != nil {
		writeError(w, r, 404, "file_not_found", "File was not found.", nil)
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		writeError(w, r, 404, "file_not_found", "File was not found.", nil)
		return
	}
	disposition := "attachment"
	if inline {
		disposition = "inline"
	}
	w.Header().Set("Content-Type", contentType)
	w.Header().Set("Content-Disposition", mime.FormatMediaType(disposition, map[string]string{"filename": name}))
	w.Header().Set("ETag", `"`+sha+`"`)
	w.Header().Set("X-Content-SHA256", sha)
	http.ServeContent(w, r, name, info.ModTime(), file)
}

func (s *Server) downloadReleaseArtifact(w http.ResponseWriter, r *http.Request) {
	version, kind := strings.TrimSpace(chi.URLParam(r, "version")), strings.TrimSpace(chi.URLParam(r, "kind"))
	var item model.ReleaseArtifact
	err := s.DB.Model(&model.ReleaseArtifact{}).
		Joins("JOIN releases ON releases.id = release_artifacts.release_id").
		Where("releases.version = ? AND releases.status = ? AND release_artifacts.kind = ?", version, model.ReleasePublished, kind).
		First(&item).Error
	if err != nil {
		writeError(w, r, 404, "artifact_not_found", "Artifact was not found.", nil)
		return
	}
	s.serveReleaseFile(w, r, item.Path, item.FileName, item.MimeType, item.SHA256, false)
}

func (s *Server) releaseScreenshot(w http.ResponseWriter, r *http.Request) {
	version := strings.TrimSpace(chi.URLParam(r, "version"))
	screenshotID, ok := parseUUIDParam(w, r, "screenshotID")
	if !ok {
		return
	}
	var item model.ReleaseScreenshot
	err := s.DB.Model(&model.ReleaseScreenshot{}).
		Joins("JOIN releases ON releases.id = release_screenshots.release_id").
		Where("releases.version = ? AND releases.status = ? AND release_screenshots.id = ?", version, model.ReleasePublished, screenshotID).
		First(&item).Error
	if err != nil {
		writeError(w, r, 404, "screenshot_not_found", "Screenshot was not found.", nil)
		return
	}
	extension := releaseImageExtension(item.MimeType)
	s.serveReleaseFile(w, r, item.Path, "vlt-release-"+screenshotID.String()+extension, item.MimeType, item.SHA256, true)
}
