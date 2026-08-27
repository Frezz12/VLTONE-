package api

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"sort"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/promptlib"
)

// The assistant's instructions: the main prompt and the playbooks it loads for
// one kind of work. The desktop fetches them at sign-in and every few hours,
// caches what it gets, and falls back to the copy compiled into the app — so
// editing a playbook here changes how the assistant writes bass without anybody
// shipping a release.
//
// Limits mirror the desktop's own (controller/ai/Prompts.hpp): a prompt is sent
// to the model on every request and paid for by the token, so an accidental
// novel in the editor must not become an enormous bill.
const (
	maxPromptBodyBytes = 64 * 1024
	maxPromptDocuments = 128
	promptKindMain     = "main"
	promptKindPlaybook = "playbook"
)

type promptDocumentPayload struct {
	ID        string   `json:"id"`
	Kind      string   `json:"kind"`
	Title     string   `json:"title"`
	UseWhen   string   `json:"use_when"`
	Tags      []string `json:"tags"`
	Body      string   `json:"body"`
	Enabled   bool     `json:"enabled"`
	UpdatedAt string   `json:"updated_at,omitempty"`
	// True when the text still matches what this build ships, so the panel can
	// show which documents have actually been edited.
	Builtin bool `json:"builtin"`
}

type promptWritePayload struct {
	ID      string   `json:"id"`
	Kind    string   `json:"kind"`
	Title   string   `json:"title"`
	UseWhen string   `json:"use_when"`
	Tags    []string `json:"tags"`
	Body    string   `json:"body"`
	Enabled *bool    `json:"enabled"`
}

func tagsJSON(tags []string) datatypes.JSON {
	if tags == nil {
		tags = []string{}
	}
	body, err := json.Marshal(tags)
	if err != nil {
		return datatypes.JSON([]byte("[]"))
	}
	return datatypes.JSON(body)
}

func tagsOf(raw datatypes.JSON) []string {
	tags := []string{}
	if len(raw) > 0 {
		_ = json.Unmarshal(raw, &tags)
	}
	return tags
}

// validPromptID keeps ids to what the desktop's parser accepts, so a document
// created here can never be one the app then refuses to load.
func validPromptID(id string) bool {
	if id == "" || len(id) > 64 {
		return false
	}
	for _, c := range id {
		if (c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '-' {
			return false
		}
	}
	return true
}

// ensurePromptSeed fills an empty table with the text this build ships. Done
// lazily rather than in the migration so the seed always matches the running
// binary, and so a deployment that rolls forward brings its prompts with it.
func (s *Server) ensurePromptSeed() error {
	var count int64
	if err := s.DB.Model(&model.AIPromptDocument{}).Count(&count).Error; err != nil {
		return err
	}
	if count > 0 {
		return nil
	}
	now := time.Now().UTC()
	documents := make([]model.AIPromptDocument, 0, len(promptlib.Builtin()))
	for _, doc := range promptlib.Builtin() {
		documents = append(documents, model.AIPromptDocument{
			ID: doc.ID, Kind: doc.Kind, Title: doc.Title, UseWhen: doc.UseWhen,
			Tags: tagsJSON(doc.Tags), Body: doc.Body, Enabled: true,
			CreatedAt: now, UpdatedAt: now,
		})
	}
	if len(documents) == 0 {
		return nil
	}
	return s.DB.Create(&documents).Error
}

func (s *Server) promptDocuments() ([]model.AIPromptDocument, error) {
	if err := s.ensurePromptSeed(); err != nil {
		return nil, err
	}
	var documents []model.AIPromptDocument
	if err := s.DB.Order("kind DESC, id ASC").Find(&documents).Error; err != nil {
		return nil, err
	}
	return documents, nil
}

// promptVersion identifies the served text. It is the ETag, it is what the
// desktop shows in Settings, and it changes on any edit — which is exactly what
// a client needs to decide whether to download again.
func promptVersion(documents []model.AIPromptDocument) string {
	ordered := make([]model.AIPromptDocument, len(documents))
	copy(ordered, documents)
	sort.Slice(ordered, func(a, b int) bool { return ordered[a].ID < ordered[b].ID })
	digest := sha256.New()
	for _, doc := range ordered {
		if !doc.Enabled {
			continue
		}
		for _, part := range []string{doc.ID, doc.Kind, doc.Title, doc.UseWhen,
			string(doc.Tags), doc.Body} {
			digest.Write([]byte(part))
			digest.Write([]byte{0})
		}
	}
	return "v" + hex.EncodeToString(digest.Sum(nil))[:12]
}

// promptPack is what the desktop reads: the same shape parsePromptPack expects.
func (s *Server) promptPack(w http.ResponseWriter, r *http.Request) {
	documents, err := s.promptDocuments()
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "prompts_unavailable",
			"The assistant's instructions could not be read.", nil)
		return
	}
	version := promptVersion(documents)
	etag := `"` + version + `"`
	// A pack is tens of kilobytes and changes rarely; without this every client
	// would re-download the whole thing several times a day for nothing.
	if match := r.Header.Get("If-None-Match"); match != "" {
		for _, candidate := range strings.Split(match, ",") {
			if strings.TrimSpace(candidate) == etag {
				w.Header().Set("ETag", etag)
				w.WriteHeader(http.StatusNotModified)
				return
			}
		}
	}

	main := ""
	playbooks := make([]promptDocumentPayload, 0, len(documents))
	for _, doc := range documents {
		if !doc.Enabled {
			continue
		}
		if doc.Kind == promptKindMain {
			// More than one "main" would be a mistake somebody made in the
			// panel; the first by id wins rather than the response being empty.
			if main == "" {
				main = doc.Body
			}
			continue
		}
		playbooks = append(playbooks, promptDocumentPayload{
			ID: doc.ID, Title: doc.Title, UseWhen: doc.UseWhen,
			Tags: tagsOf(doc.Tags), Body: doc.Body,
		})
	}
	if main == "" {
		// Never serve a pack with no main prompt: the desktop would reject it
		// and stay on its built-in copy anyway, so say so honestly instead.
		writeError(w, r, http.StatusServiceUnavailable, "prompts_incomplete",
			"The assistant's main instructions are missing on this server.", nil)
		return
	}
	w.Header().Set("ETag", etag)
	writeJSON(w, http.StatusOK, map[string]any{
		"version": version, "main": main, "playbooks": playbooks,
	})
}

// ── Admin ───────────────────────────────────────────────────────────────────

func promptPayload(doc model.AIPromptDocument) promptDocumentPayload {
	payload := promptDocumentPayload{
		ID: doc.ID, Kind: doc.Kind, Title: doc.Title, UseWhen: doc.UseWhen,
		Tags: tagsOf(doc.Tags), Body: doc.Body, Enabled: doc.Enabled,
		UpdatedAt: doc.UpdatedAt.UTC().Format(time.RFC3339),
	}
	if builtin, ok := promptlib.Find(doc.ID); ok {
		payload.Builtin = builtin.Body == doc.Body
	}
	return payload
}

func (s *Server) adminPrompts(w http.ResponseWriter, r *http.Request) {
	documents, err := s.promptDocuments()
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "prompts_unavailable",
			"The assistant's instructions could not be read.", nil)
		return
	}
	payloads := make([]promptDocumentPayload, 0, len(documents))
	for _, doc := range documents {
		payloads = append(payloads, promptPayload(doc))
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"version": promptVersion(documents), "prompts": payloads,
	})
}

func (s *Server) adminPrompt(w http.ResponseWriter, r *http.Request) {
	doc, ok := s.loadPrompt(w, r)
	if !ok {
		return
	}
	writeJSON(w, http.StatusOK, promptPayload(doc))
}

func (s *Server) loadPrompt(w http.ResponseWriter, r *http.Request) (model.AIPromptDocument, bool) {
	if err := s.ensurePromptSeed(); err != nil {
		writeError(w, r, http.StatusInternalServerError, "prompts_unavailable",
			"The assistant's instructions could not be read.", nil)
		return model.AIPromptDocument{}, false
	}
	id := chi.URLParam(r, "promptID")
	var doc model.AIPromptDocument
	err := s.DB.Where("id = ?", id).First(&doc).Error
	if errors.Is(err, gorm.ErrRecordNotFound) {
		writeError(w, r, http.StatusNotFound, "prompt_not_found",
			"There is no such instruction document.", nil)
		return model.AIPromptDocument{}, false
	}
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "prompts_unavailable",
			"The assistant's instructions could not be read.", nil)
		return model.AIPromptDocument{}, false
	}
	return doc, true
}

// recordPromptRevision keeps the text an edit is about to replace.
func (s *Server) recordPromptRevision(tx *gorm.DB, doc model.AIPromptDocument, admin uuid.UUID) error {
	return tx.Create(&model.AIPromptRevision{
		ID: uuid.New(), DocumentID: doc.ID, Title: doc.Title, UseWhen: doc.UseWhen,
		Tags: doc.Tags, Body: doc.Body, UpdatedBy: &admin, CreatedAt: time.Now().UTC(),
	}).Error
}

func (s *Server) adminUpdatePrompt(w http.ResponseWriter, r *http.Request) {
	doc, ok := s.loadPrompt(w, r)
	if !ok {
		return
	}
	var input promptWritePayload
	if !decodeJSON(w, r, &input) {
		return
	}
	if strings.TrimSpace(input.Body) == "" {
		writeError(w, r, http.StatusUnprocessableEntity, "prompt_invalid",
			"An instruction document cannot be empty.",
			map[string]string{"body": "Write the instructions."})
		return
	}
	if len(input.Body) > maxPromptBodyBytes {
		writeError(w, r, http.StatusUnprocessableEntity, "prompt_too_long",
			"That document is too long to send to the model on every request.",
			map[string]string{"body": "Keep it under 64 KB."})
		return
	}

	admin := adminFrom(r).ID
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := s.recordPromptRevision(tx, doc, admin); err != nil {
			return err
		}
		updates := map[string]any{
			"title": input.Title, "use_when": input.UseWhen,
			"tags": tagsJSON(input.Tags), "body": input.Body,
			"updated_by": admin, "updated_at": time.Now().UTC(),
		}
		if input.Enabled != nil {
			updates["enabled"] = *input.Enabled
		}
		if err := tx.Model(&model.AIPromptDocument{}).Where("id = ?", doc.ID).
			Updates(updates).Error; err != nil {
			return err
		}
		// The document id is not personal data, so it goes in the metadata
		// where it stays readable, rather than through targetHash.
		return s.audit(tx, r, "ai_prompt_update", "ai_prompt", uuid.Nil,
			map[string]any{"document": doc.ID})
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "prompt_not_saved",
			"That document could not be saved.", nil)
		return
	}
	s.respondWithPrompt(w, r, doc.ID)
}

func (s *Server) adminCreatePrompt(w http.ResponseWriter, r *http.Request) {
	var input promptWritePayload
	if !decodeJSON(w, r, &input) {
		return
	}
	if !validPromptID(input.ID) {
		writeError(w, r, http.StatusUnprocessableEntity, "prompt_invalid",
			"An id may hold lower-case letters, digits and dashes only.",
			map[string]string{"id": "Use something like drum-fills."})
		return
	}
	if strings.TrimSpace(input.Body) == "" || len(input.Body) > maxPromptBodyBytes {
		writeError(w, r, http.StatusUnprocessableEntity, "prompt_invalid",
			"A new document needs instructions, and under 64 KB of them.",
			map[string]string{"body": "Write the instructions."})
		return
	}
	if err := s.ensurePromptSeed(); err != nil {
		writeError(w, r, http.StatusInternalServerError, "prompts_unavailable",
			"The assistant's instructions could not be read.", nil)
		return
	}
	var count int64
	if err := s.DB.Model(&model.AIPromptDocument{}).Count(&count).Error; err == nil &&
		count >= maxPromptDocuments {
		writeError(w, r, http.StatusUnprocessableEntity, "prompt_limit",
			"There are already as many instruction documents as the app will load.", nil)
		return
	}

	now := time.Now().UTC()
	admin := adminFrom(r).ID
	doc := model.AIPromptDocument{
		ID: input.ID, Kind: promptKindPlaybook, Title: input.Title,
		UseWhen: input.UseWhen, Tags: tagsJSON(input.Tags), Body: input.Body,
		Enabled: true, UpdatedBy: &admin, CreatedAt: now, UpdatedAt: now,
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Create(&doc).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "ai_prompt_create", "ai_prompt", uuid.Nil,
			map[string]any{"document": doc.ID})
	})
	if err != nil {
		writeError(w, r, http.StatusConflict, "prompt_exists",
			"A document with that id already exists.", nil)
		return
	}
	writeJSON(w, http.StatusCreated, promptPayload(doc))
}

func (s *Server) adminDeletePrompt(w http.ResponseWriter, r *http.Request) {
	doc, ok := s.loadPrompt(w, r)
	if !ok {
		return
	}
	if doc.Kind == promptKindMain {
		writeError(w, r, http.StatusUnprocessableEntity, "prompt_required",
			"The main instructions cannot be deleted. Edit them instead.", nil)
		return
	}
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Delete(&model.AIPromptDocument{}, "id = ?", doc.ID).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "ai_prompt_delete", "ai_prompt", uuid.Nil,
			map[string]any{"document": doc.ID})
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "prompt_not_deleted",
			"That document could not be deleted.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"deleted": doc.ID})
}

// adminRevertPrompt puts back the text this build of the app ships with. The
// safety net under editing behaviour: whatever an edit broke, this is known to
// have worked.
func (s *Server) adminRevertPrompt(w http.ResponseWriter, r *http.Request) {
	doc, ok := s.loadPrompt(w, r)
	if !ok {
		return
	}
	builtin, found := promptlib.Find(doc.ID)
	if !found {
		writeError(w, r, http.StatusUnprocessableEntity, "prompt_not_builtin",
			"This document was added here, so there is no built-in text to go back to.", nil)
		return
	}
	admin := adminFrom(r).ID
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := s.recordPromptRevision(tx, doc, admin); err != nil {
			return err
		}
		if err := tx.Model(&model.AIPromptDocument{}).Where("id = ?", doc.ID).
			Updates(map[string]any{
				"title": builtin.Title, "use_when": builtin.UseWhen,
				"tags": tagsJSON(builtin.Tags), "body": builtin.Body,
				"updated_by": admin, "updated_at": time.Now().UTC(),
			}).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "ai_prompt_revert", "ai_prompt", uuid.Nil,
			map[string]any{"document": doc.ID})
	})
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "prompt_not_saved",
			"That document could not be restored.", nil)
		return
	}
	s.respondWithPrompt(w, r, doc.ID)
}

func (s *Server) adminPromptRevisions(w http.ResponseWriter, r *http.Request) {
	doc, ok := s.loadPrompt(w, r)
	if !ok {
		return
	}
	var revisions []model.AIPromptRevision
	if err := s.DB.Where("document_id = ?", doc.ID).
		Order("created_at DESC").Limit(50).Find(&revisions).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "prompts_unavailable",
			"That document's history could not be read.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"revisions": revisions})
}

func (s *Server) respondWithPrompt(w http.ResponseWriter, r *http.Request, id string) {
	var doc model.AIPromptDocument
	if err := s.DB.Where("id = ?", id).First(&doc).Error; err != nil {
		writeJSON(w, http.StatusOK, map[string]any{"id": id})
		return
	}
	writeJSON(w, http.StatusOK, promptPayload(doc))
}
