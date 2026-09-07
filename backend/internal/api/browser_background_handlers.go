package api

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"image"
	"image/jpeg"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/google/uuid"
	"golang.org/x/image/draw"
	"gorm.io/gorm"
	"vltstudio/backend/internal/model"
)

const maxBrowserBackgroundBytes = 20 << 20
const maxBrowserBackgrounds = 100

type browserBackgroundView struct {
	model.BrowserBackground
	URL          string `json:"url"`
	ThumbnailURL string `json:"thumbnail_url"`
}

func backgroundView(item model.BrowserBackground, admin bool) browserBackgroundView {
	prefix := "/v1/browser-backgrounds/"
	if admin {
		prefix = "/v1/admin/browser-backgrounds/"
	}
	return browserBackgroundView{item, prefix + item.ID.String() + "/image", prefix + item.ID.String() + "/thumbnail"}
}
func (s *Server) listBrowserBackgrounds(w http.ResponseWriter, r *http.Request, admin bool) {
	var items []model.BrowserBackground
	query := s.DB.WithContext(r.Context())
	if !admin {
		query = query.Where("published = ?", true)
	}
	if err := query.Order("sort_order, created_at DESC, id").Limit(maxBrowserBackgrounds).Find(&items).Error; err != nil {
		writeError(w, r, 500, "backgrounds_unavailable", "Background collection is unavailable.", nil)
		return
	}
	result := make([]browserBackgroundView, 0, len(items))
	for _, item := range items {
		result = append(result, backgroundView(item, admin))
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, 200, map[string]any{"backgrounds": result})
}
func (s *Server) publicBrowserBackgrounds(w http.ResponseWriter, r *http.Request) {
	s.listBrowserBackgrounds(w, r, false)
}
func (s *Server) adminBrowserBackgrounds(w http.ResponseWriter, r *http.Request) {
	s.listBrowserBackgrounds(w, r, true)
}

func (s *Server) backgroundDirectory(id uuid.UUID) string {
	return filepath.Join(s.Config.StorageRoot, "browser-backgrounds", id.String())
}

func (s *Server) adminUploadBrowserBackground(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, (10<<20)+(1<<20))
	if err := r.ParseMultipartForm(1 << 20); err != nil {
		writeError(w, r, 400, "background_invalid", "Choose an image no larger than 10 MB.", nil)
		return
	}
	defer r.MultipartForm.RemoveAll()
	title := strings.TrimSpace(r.FormValue("title"))
	files := r.MultipartForm.File["file"]
	if title == "" || len([]rune(title)) > 160 || len(files) != 1 {
		writeError(w, r, 422, "background_invalid", "A title (up to 160 characters) and one image are required.", nil)
		return
	}
	body, err := multipartFile(files[0], 10<<20)
	if err != nil {
		writeError(w, r, 422, "background_invalid", "Image exceeds 10 MB.", nil)
		return
	}
	// Re-encode to remove metadata and ensure only decoded image content is served.
	body, mime, ext, err := sanitizeImageWithPixelLimit(body, 16_000_000)
	if err != nil || len(body) > maxBrowserBackgroundBytes {
		writeError(w, r, 422, "background_invalid", "Use a valid PNG, JPEG or WebP image up to 16 megapixels.", nil)
		return
	}
	thumbnail, width, height, err := browserBackgroundThumbnail(body)
	if err != nil {
		writeError(w, r, 422, "background_invalid", "Preview could not be created; try uploading again.", nil)
		return
	}
	id := uuid.New()
	directory := s.backgroundDirectory(id)
	if err = os.MkdirAll(directory, 0o700); err != nil {
		writeError(w, r, 500, "background_save_failed", "Image storage is unavailable.", nil)
		return
	}
	keep := false
	defer func() {
		if !keep {
			_ = os.RemoveAll(directory)
		}
	}()
	if err = os.WriteFile(filepath.Join(directory, "image"+ext), body, 0o600); err == nil {
		err = os.WriteFile(filepath.Join(directory, "thumbnail.jpg"), thumbnail, 0o600)
	}
	if err != nil {
		writeError(w, r, 500, "background_save_failed", "Image could not be saved.", nil)
		return
	}
	digest := sha256.Sum256(body)
	item := model.BrowserBackground{ID: id, Title: title, Published: true, MimeType: mime, Extension: ext, Bytes: int64(len(body)), Width: width, Height: height, SHA256: hex.EncodeToString(digest[:])}
	err = s.DB.WithContext(r.Context()).Transaction(func(tx *gorm.DB) error {
		if err := tx.Exec("SELECT pg_advisory_xact_lock(hashtext('browser-backgrounds'))").Error; err != nil {
			return err
		}
		var count int64
		if err := tx.Model(&model.BrowserBackground{}).Count(&count).Error; err != nil {
			return err
		}
		if count >= maxBrowserBackgrounds {
			return errors.New("background collection is full (100 images)")
		}
		if err := tx.Create(&item).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "browser_background.upload", "browser_background", id, map[string]any{"sha256": item.SHA256})
	})
	if err != nil {
		writeError(w, r, 422, "background_save_failed", "Image could not be added. The collection holds up to 100 images.", nil)
		return
	}
	keep = true
	writeJSON(w, 201, backgroundView(item, true))
}

// Use the same decoder budget as uploads, including the second decode for previews.
func browserBackgroundThumbnail(body []byte) ([]byte, int, int, error) {
	select {
	case imageDecodeSlots <- struct{}{}:
		defer func() { <-imageDecodeSlots }()
	default:
		return nil, 0, 0, errors.New("image decoder busy")
	}
	decoded, _, err := image.Decode(bytes.NewReader(body))
	if err != nil {
		return nil, 0, 0, err
	}
	bounds := decoded.Bounds()
	width, height := bounds.Dx(), bounds.Dy()
	w, h := min(480, width), max(1, height*min(480, width)/width)
	if h > 300 {
		w, h = max(1, w*300/h), 300
	}
	thumb := image.NewRGBA(image.Rect(0, 0, w, h))
	draw.ApproxBiLinear.Scale(thumb, thumb.Bounds(), decoded, bounds, draw.Src, nil)
	var output bytes.Buffer
	err = jpeg.Encode(&output, thumb, &jpeg.Options{Quality: 80})
	return output.Bytes(), width, height, err
}

type browserBackgroundInput struct {
	Title     string `json:"title"`
	Published bool   `json:"published"`
	SortOrder int    `json:"sort_order"`
}

func (s *Server) adminUpdateBrowserBackground(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "backgroundID")
	if !ok {
		return
	}
	var input browserBackgroundInput
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Title = strings.TrimSpace(input.Title)
	if input.Title == "" || len([]rune(input.Title)) > 160 || input.SortOrder < 0 || input.SortOrder > 100000 {
		writeError(w, r, 422, "validation_failed", "Provide a title and an order between 0 and 100000.", nil)
		return
	}
	var item model.BrowserBackground
	err := s.DB.WithContext(r.Context()).Transaction(func(tx *gorm.DB) error {
		result := tx.Model(&model.BrowserBackground{}).Where("id = ?", id).Updates(map[string]any{"title": input.Title, "published": input.Published, "sort_order": input.SortOrder})
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected == 0 {
			return gorm.ErrRecordNotFound
		}
		if err := tx.First(&item, "id = ?", id).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "browser_background.update", "browser_background", id, map[string]any{"published": input.Published})
	})
	if err != nil {
		s.backgroundError(w, r, err)
		return
	}
	writeJSON(w, 200, backgroundView(item, true))
}
func (s *Server) adminDeleteBrowserBackground(w http.ResponseWriter, r *http.Request) {
	id, ok := parseUUIDParam(w, r, "backgroundID")
	if !ok {
		return
	}
	err := s.DB.WithContext(r.Context()).Transaction(func(tx *gorm.DB) error {
		result := tx.Delete(&model.BrowserBackground{}, "id = ?", id)
		if result.Error != nil {
			return result.Error
		}
		if result.RowsAffected == 0 {
			return gorm.ErrRecordNotFound
		}
		return s.audit(tx, r, "browser_background.delete", "browser_background", id, nil)
	})
	if err != nil {
		s.backgroundError(w, r, err)
		return
	}
	_ = os.RemoveAll(s.backgroundDirectory(id))
	w.WriteHeader(204)
}
func (s *Server) backgroundError(w http.ResponseWriter, r *http.Request, err error) {
	if errors.Is(err, gorm.ErrRecordNotFound) {
		writeError(w, r, 404, "background_not_found", "Background was not found.", nil)
	} else {
		writeError(w, r, 500, "background_unavailable", "Background is unavailable.", nil)
	}
}
func (s *Server) serveBrowserBackground(w http.ResponseWriter, r *http.Request, admin, thumbnail bool) {
	id, ok := parseUUIDParam(w, r, "backgroundID")
	if !ok {
		return
	}
	var item model.BrowserBackground
	query := s.DB.WithContext(r.Context()).Where("id = ?", id)
	if !admin {
		query = query.Where("published = ?", true)
	}
	if err := query.First(&item).Error; err != nil {
		s.backgroundError(w, r, err)
		return
	}
	name, mime := "image"+item.Extension, item.MimeType
	if item.Extension != ".jpg" && item.Extension != ".png" {
		s.backgroundError(w, r, fmt.Errorf("invalid extension"))
		return
	}
	if thumbnail {
		name = "thumbnail.jpg"
		mime = "image/jpeg"
	}
	file, err := os.Open(filepath.Join(s.backgroundDirectory(id), name))
	if err != nil {
		s.backgroundError(w, r, err)
		return
	}
	defer file.Close()
	stat, err := file.Stat()
	if err != nil {
		s.backgroundError(w, r, err)
		return
	}
	w.Header().Set("Content-Type", mime)
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "private, max-age=0, must-revalidate")
	w.Header().Set("ETag", fmt.Sprintf(`"%s-%t"`, item.SHA256, thumbnail))
	http.ServeContent(w, r, name, stat.ModTime(), file)
}
