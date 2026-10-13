package api

import (
	"crypto/subtle"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"mime"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
	"vltstudio/backend/internal/auth"
	"vltstudio/backend/internal/forumfiles"
	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/objectstore"
)

const forumPartBytes int64 = 16 * 1024 * 1024

func forumFileType(name string) (kind, resource, contentType string) {
	lower := strings.ToLower(name)
	for ext, typ := range map[string]string{".vltmini": "mini", ".vltnode": "node", ".vltcreator": "creator", ".vlts": "strip", ".vltmorph": "morph", ".vltslicer": "slicer", ".vlttheme": "theme", ".vltlang.json": "language", ".vlt": "project", ".vlt.zip": "project-package", ".vltt.zip": "template", ".vltt": "template-document"} {
		if strings.HasSuffix(lower, ext) {
			return "vlt", typ, "application/octet-stream"
		}
	}
	switch strings.ToLower(filepath.Ext(name)) {
	case ".jpg", ".jpeg":
		return "image", "", "image/jpeg"
	case ".png":
		return "image", "", "image/png"
	case ".webp":
		return "image", "", "image/webp"
	case ".gif":
		return "image", "", "image/gif"
	case ".mp4":
		return "video", "", "video/mp4"
	case ".mov":
		return "video", "", "video/quicktime"
	case ".webm":
		return "video", "", "video/webm"
	}
	return "", "", ""
}
func (s *Server) forumUploadCreate(w http.ResponseWriter, r *http.Request) {
	if !s.forumAllow(w, r, "upload", 60, time.Hour) {
		return
	}
	if s.ForumFiles == nil {
		writeError(w, r, 503, "storage_unavailable", "Storage is unavailable.", nil)
		return
	}
	var in struct {
		Name           string `json:"name"`
		Bytes          int64  `json:"bytes"`
		SHA256         string `json:"sha256"`
		ResourceType   string `json:"resource_type"`
		MinimumVersion string `json:"minimum_version"`
	}
	if !decodeJSON(w, r, &in) {
		return
	}
	kind, resource, contentType := forumFileType(in.Name)
	hash, hashErr := hex.DecodeString(in.SHA256)
	limit := s.Config.ForumFileMaxBytes
	if kind == "image" {
		limit = s.Config.ForumImageMaxBytes
	}
	if kind == "video" {
		limit = s.Config.ForumVideoMaxBytes
	}
	if kind == "" || in.Name != filepath.Base(in.Name) || strings.ContainsAny(in.Name, "\\\r\n\x00") || len(in.Name) > 240 || in.Bytes <= 0 || in.Bytes > limit || hashErr != nil || len(hash) != 32 || strings.ToLower(in.SHA256) != in.SHA256 || len(in.MinimumVersion) > 40 {
		forumFailure(w, r, errForumInput)
		return
	}
	if resource == "strip" && in.ResourceType == "offline-strip" {
		resource = "offline-strip"
	}
	raw, e := auth.RandomToken(32)
	if forumFailure(w, r, e) {
		return
	}
	id := uuid.New()
	now := time.Now().UTC()
	a := model.ForumAttachment{ID: id, OwnerID: userFrom(r).ID, Name: in.Name, Bytes: in.Bytes, SHA256: in.SHA256, ContentType: contentType, Kind: kind, ResourceType: resource, MinimumVersion: in.MinimumVersion, Status: "uploading", MediaStatus: "none", ObjectKey: "objects/" + id.String(), PartBytes: forumPartBytes, UploadTokenHash: auth.HashToken(raw), ExpiresAt: now.Add(24 * time.Hour)}
	e = s.DB.Transaction(func(tx *gorm.DB) error {
		var u model.User
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&u, "id = ?", a.OwnerID).Error; err != nil {
			return err
		}
		var total int64
		var active int64
		tx.Model(&model.ForumAttachment{}).Where("owner_id = ? AND status <> 'cancelled'", a.OwnerID).Select("COALESCE(sum(bytes),0)").Scan(&total)
		tx.Model(&model.ForumAttachment{}).Where("owner_id = ? AND status = 'uploading'", a.OwnerID).Count(&active)
		if total+a.Bytes > s.Config.ForumUserMaxBytes || active >= 20 {
			return errForumInput
		}
		if s.ForumFiles.Remote != nil {
			uploadID, err := s.ForumFiles.Remote.CreateMultipart(r.Context(), "staging/"+id.String(), objectstore.ExpectedObject{SHA256: a.SHA256, Bytes: a.Bytes, ContentType: a.ContentType})
			if err != nil {
				return err
			}
			a.ProviderUploadID = uploadID
		}
		return tx.Create(&a).Error
	})
	if forumFailure(w, r, e) {
		if a.ProviderUploadID != "" {
			s.ForumFiles.Remote.AbortMultipart(r.Context(), "staging/"+id.String(), a.ProviderUploadID)
		}
		return
	}
	s.forumUploadResponse(w, r, a, raw)
}
func (s *Server) forumUploadResponse(w http.ResponseWriter, r *http.Request, a model.ForumAttachment, token string) {
	type part struct {
		Number  int                           `json:"number"`
		Bytes   int64                         `json:"bytes"`
		Done    bool                          `json:"done"`
		Request *objectstore.PresignedRequest `json:"request,omitempty"`
	}
	parts := []part{}
	observed := map[int]objectstore.UploadedPart{}
	completed := false
	if s.ForumFiles.Remote != nil && a.Status == "uploading" {
		rows, e := s.ForumFiles.Remote.ListMultipartParts(r.Context(), "staging/"+a.ID.String(), a.ProviderUploadID, 1000)
		completed = errors.Is(e, objectstore.ErrMultipartNotFound)
		if e != nil && !completed {
			forumFailure(w, r, e)
			return
		}
		for _, p := range rows {
			observed[p.PartNumber] = p
		}
	}
	for offset, n := int64(0), 1; offset < a.Bytes; offset, n = offset+a.PartBytes, n+1 {
		size := min(a.PartBytes, a.Bytes-offset)
		p := part{Number: n, Bytes: size}
		if a.Status != "uploading" || completed {
			p.Done = a.Status == "ready" || completed
			parts = append(parts, p)
			continue
		}
		if s.ForumFiles.Remote != nil {
			if o, ok := observed[n]; ok && o.Bytes == size {
				p.Done = true
			} else {
				req, e := s.ForumFiles.Remote.PresignMultipartPart(r.Context(), "staging/"+a.ID.String(), a.ProviderUploadID, n, size, 15*time.Minute)
				if forumFailure(w, r, e) {
					return
				}
				p.Request = &req
			}
		} else {
			path, _ := s.ForumFiles.Path(fmt.Sprintf("staging/%s/%d", a.ID, n))
			if info, e := os.Stat(path); e == nil && info.Size() == size {
				p.Done = true
			} else {
				p.Request = &objectstore.PresignedRequest{Method: "PUT", URL: fmt.Sprintf("%s/v1/forum/uploads/%s/data/%d?token=%s", s.Config.DesktopAPIOrigin, a.ID, n, url.QueryEscape(token)), Headers: map[string]string{"Content-Type": "application/octet-stream"}, ExpiresAt: a.ExpiresAt}
			}
		}
		parts = append(parts, p)
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, 200, map[string]any{"attachment": a, "parts": parts})
}
func (s *Server) forumUploadResume(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "attachmentID")
	if !ok {
		return
	}
	var a model.ForumAttachment
	if forumFailure(w, r, s.DB.First(&a, "id = ? AND owner_id = ?", id, userFrom(r).ID).Error) {
		return
	}
	if a.Status == "uploading" && a.ExpiresAt.Before(time.Now()) {
		forumFailure(w, r, errForumInput)
		return
	}
	raw, e := auth.RandomToken(32)
	if forumFailure(w, r, e) {
		return
	}
	if forumFailure(w, r, s.DB.Model(&a).Update("upload_token_hash", auth.HashToken(raw)).Error) {
		return
	}
	s.forumUploadResponse(w, r, a, raw)
}
func (s *Server) forumUploadData(w http.ResponseWriter, r *http.Request) {
	origin := r.Header.Get("Origin")
	if origin == s.Config.ForumOrigin {
		w.Header().Set("Access-Control-Allow-Origin", origin)
		w.Header().Set("Vary", "Origin")
		w.Header().Set("Access-Control-Allow-Methods", "PUT, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
	}
	if r.Method == "OPTIONS" {
		w.WriteHeader(204)
		return
	}
	if s.ForumFiles == nil || s.ForumFiles.Remote != nil {
		http.NotFound(w, r)
		return
	}
	id, ok := parseUUIDParam(w, r, "attachmentID")
	if !ok {
		return
	}
	n, e := strconv.Atoi(chi.URLParam(r, "part"))
	if e != nil || n < 1 {
		http.NotFound(w, r)
		return
	}
	var a model.ForumAttachment
	if s.DB.First(&a, "id = ? AND status = 'uploading' AND expires_at > ?", id, time.Now()).Error != nil {
		http.NotFound(w, r)
		return
	}
	hash := auth.HashToken(r.URL.Query().Get("token"))
	if subtle.ConstantTimeCompare([]byte(hash), []byte(a.UploadTokenHash)) != 1 {
		http.Error(w, "Upload expired", 403)
		return
	}
	offset := int64(n-1) * a.PartBytes
	size := min(a.PartBytes, a.Bytes-offset)
	if size <= 0 {
		http.Error(w, "Invalid part", 422)
		return
	}
	path, _ := s.ForumFiles.Path(fmt.Sprintf("staging/%s/%d", id, n))
	if e = os.MkdirAll(filepath.Dir(path), 0700); e != nil {
		http.Error(w, "Storage unavailable", 503)
		return
	}
	f, e := os.CreateTemp(filepath.Dir(path), ".part-")
	if e != nil {
		http.Error(w, "Storage unavailable", 503)
		return
	}
	defer os.Remove(f.Name())
	written, e := io.Copy(f, http.MaxBytesReader(w, r.Body, size))
	closeErr := f.Close()
	if e != nil || closeErr != nil || written != size {
		http.Error(w, "Incomplete part", 422)
		return
	}
	if e = os.Rename(f.Name(), path); e != nil {
		http.Error(w, "Storage unavailable", 503)
		return
	}
	w.WriteHeader(204)
}
func (s *Server) forumUploadComplete(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "attachmentID")
	if !ok {
		return
	}
	var a model.ForumAttachment
	// The row lock serializes completion, cancellation and repeat requests.
	err := s.DB.WithContext(r.Context()).Transaction(func(tx *gorm.DB) error {
		if e := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&a, "id = ? AND owner_id = ?", id, userFrom(r).ID).Error; e != nil {
			return e
		}
		if a.Status == "ready" {
			return nil
		}
		if a.Status != "uploading" || a.ExpiresAt.Before(time.Now()) {
			return errForumInput
		}
		staging := "staging/" + id.String()
		expected := objectstore.ExpectedObject{SHA256: a.SHA256, Bytes: a.Bytes, ContentType: a.ContentType}
		if s.ForumFiles.Remote != nil {
			parts, e := s.ForumFiles.Remote.ListMultipartParts(r.Context(), staging, a.ProviderUploadID, 1000)
			// A completed multipart object survives a process/DB failure. Resume
			// by verifying the immutable staged bytes again, never trusting ETags.
			if e != nil && !errors.Is(e, objectstore.ErrMultipartNotFound) {
				return e
			}
			if e == nil {
				if len(parts) != int((a.Bytes+a.PartBytes-1)/a.PartBytes) {
					return errForumInput
				}
				for i, p := range parts {
					if p.PartNumber != i+1 || p.Bytes != min(a.PartBytes, a.Bytes-int64(i)*a.PartBytes) {
						return errForumInput
					}
				}
				if e = s.ForumFiles.Remote.CompleteMultipart(r.Context(), staging, a.ProviderUploadID, parts); e != nil && !errors.Is(e, objectstore.ErrMultipartNotFound) {
					return e
				}
			}
			if e = s.ForumFiles.Remote.VerifyAndPromote(r.Context(), staging, a.ObjectKey, expected); e != nil {
				return e
			}
		} else {
			tmp, e := os.CreateTemp(s.ForumFiles.Root, ".assemble-")
			if e != nil {
				return e
			}
			defer os.Remove(tmp.Name())
			defer tmp.Close()
			for offset, n := int64(0), 1; offset < a.Bytes; offset, n = offset+a.PartBytes, n+1 {
				path, _ := s.ForumFiles.Path(fmt.Sprintf("%s/%d", staging, n))
				in, e := os.Open(path)
				if e != nil {
					return e
				}
				written, e := io.Copy(tmp, io.LimitReader(in, min(a.PartBytes, a.Bytes-offset)+1))
				in.Close()
				if e != nil {
					return e
				}
				if written != min(a.PartBytes, a.Bytes-offset) {
					return errForumInput
				}
			}
			if e = tmp.Close(); e != nil {
				return e
			}
			hash, n, e := forumfiles.Hash(tmp.Name())
			if e != nil {
				return e
			}
			if hash != a.SHA256 || n != a.Bytes {
				return errForumInput
			}
			if e = s.ForumFiles.Put(r.Context(), a.ObjectKey, tmp.Name(), a.ContentType); e != nil {
				return e
			}
		}
		a.Status = "ready"
		if a.Kind == "video" {
			a.MediaStatus = "pending"
			if e := tx.Clauses(clause.OnConflict{DoNothing: true}).Create(&model.ForumMediaJob{AttachmentID: a.ID, AvailableAt: time.Now().UTC()}).Error; e != nil {
				return e
			}
		}
		return tx.Save(&a).Error
	})
	if !forumFailure(w, r, err) {
		if s.ForumFiles.Remote != nil {
			_ = s.ForumFiles.Remote.Delete(r.Context(), "staging/"+a.ID.String())
		} else {
			path, _ := s.ForumFiles.Path("staging/" + a.ID.String())
			_ = os.RemoveAll(path)
		}
		writeJSON(w, 200, map[string]any{"attachment": a})
	}
}
func (s *Server) forumUploadCancel(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "attachmentID")
	if !ok {
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		var a model.ForumAttachment
		if e := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&a, "id = ? AND owner_id = ?", id, userFrom(r).ID).Error; e != nil {
			return e
		}
		if a.ArticleID != nil || a.TopicID != nil || a.PostID != nil || a.CommentID != nil {
			return errForumInput
		}
		if s.ForumFiles.Remote != nil && a.ProviderUploadID != "" {
			s.ForumFiles.Remote.AbortMultipart(r.Context(), "staging/"+id.String(), a.ProviderUploadID)
		}
		return tx.Delete(&a).Error
	})
	if !forumFailure(w, r, err) {
		w.WriteHeader(204)
	}
}
func (s *Server) forumContentAttachments(kind string, id uuid.UUID) []model.ForumAttachment {
	rows := []model.ForumAttachment{}
	column := map[string]string{"article": "article_id", "topic": "topic_id", "post": "post_id", "comment": "comment_id"}[kind]
	if column != "" {
		s.DB.Where(column+" = ? AND status = 'ready'", id).Order("created_at,id").Find(&rows)
	}
	return rows
}
func (s *Server) forumAttachmentVisible(r *http.Request, a model.ForumAttachment) bool {
	if a.Status != "ready" {
		return false
	}
	for kind, id := range map[string]*uuid.UUID{"article": a.ArticleID, "topic": a.TopicID, "post": a.PostID, "comment": a.CommentID} {
		if id != nil {
			_, e := forumTarget(s.DB, kind, *id, viewerID(r), false)
			return e == nil
		}
	}
	if viewerID(r) == a.OwnerID {
		return true
	}
	var n int64
	s.DB.Model(&model.ForumProfile{}).Where("avatar_id = ?", a.ID).Count(&n)
	return n > 0 && a.Kind == "image"
}
func (s *Server) forumAttachment(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "attachmentID")
	if !ok {
		return
	}
	var a model.ForumAttachment
	if s.DB.First(&a, "id = ?", id).Error != nil || !s.forumAttachmentVisible(r, a) {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	if chi.URLParam(r, "variant") == "" && !strings.HasSuffix(r.URL.Path, "/download") {
		writeJSON(w, 200, map[string]any{"attachment": a})
		return
	}
	if a.MediaStatus == "rejected" {
		http.NotFound(w, r)
		return
	}
	key := a.ObjectKey
	contentType := a.ContentType
	variant := chi.URLParam(r, "variant")
	if variant != "" && variant != "original" {
		if a.MediaStatus != "ready" || a.MediaKey == "" {
			http.NotFound(w, r)
			return
		}
		types := map[string]string{"poster.jpg": "image/jpeg", "video.mp4": "video/mp4", "video.webm": "video/webm"}
		var exists bool
		contentType, exists = types[variant]
		if !exists {
			http.NotFound(w, r)
			return
		}
		key = a.MediaKey + "/" + variant
	}
	if variant == "original" && a.Kind == "vlt" {
		http.NotFound(w, r)
		return
	}
	if variant == "" {
		w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": a.Name}))
		contentType = "application/octet-stream"
	}
	w.Header().Set("Content-Type", contentType)
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Content-Security-Policy", "default-src 'none'; sandbox")
	s.ForumFiles.Serve(w, r, key)
}

// Removing a published attachment is an explicit author action, including
// replacement of rejected videos. Serialize on the parent before the file.
func (s *Server) forumAttachmentDelete(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "attachmentID")
	if !ok {
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		var a model.ForumAttachment
		if e := tx.First(&a, "id = ? AND owner_id = ?", id, userFrom(r).ID).Error; e != nil {
			return e
		}
		for kind, parent := range map[string]*uuid.UUID{"article": a.ArticleID, "topic": a.TopicID, "post": a.PostID, "comment": a.CommentID} {
			if parent == nil {
				continue
			}
			if _, e := forumTarget(tx, kind, *parent, userFrom(r).ID, true); e != nil {
				return e
			}
			table, _ := forumTargetTable(kind)
			var content struct {
				Body   string
				Status string
			}
			if e := tx.Table(table).Where("id = ?", *parent).Take(&content).Error; e != nil {
				return e
			}
			if content.Status == "hidden" {
				return errForumInput
			}
			if strings.TrimSpace(content.Body) == "" {
				var count int64
				tx.Model(&model.ForumAttachment{}).Where(map[string]string{"article": "article_id", "topic": "topic_id", "post": "post_id", "comment": "comment_id"}[kind]+" = ?", *parent).Count(&count)
				if count <= 1 {
					return errForumInput
				}
			}
		}
		if e := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&a, "id = ? AND owner_id = ?", id, userFrom(r).ID).Error; e != nil {
			return e
		}
		return tx.Delete(&a).Error
	})
	if !forumFailure(w, r, err) {
		w.WriteHeader(204)
	}
}
