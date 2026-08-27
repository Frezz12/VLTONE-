package api

import (
	"errors"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/google/uuid"
	"gorm.io/gorm"

	"vltstudio/backend/internal/model"
)

const maxAIModels = 100

type aiModelWritePayload struct {
	DisplayName string `json:"display_name"`
	Provider    string `json:"provider"`
	Model       string `json:"model"`
	EndpointURL string `json:"endpoint_url"`
	APIKey      string `json:"api_key"`
	Enabled     *bool  `json:"enabled,omitempty"`
	SortOrder   int    `json:"sort_order"`
}

type adminAIModelPayload struct {
	ID          uuid.UUID `json:"id"`
	DisplayName string    `json:"display_name"`
	Provider    string    `json:"provider"`
	Model       string    `json:"model"`
	EndpointURL string    `json:"endpoint_url"`
	HasAPIKey   bool      `json:"has_api_key"`
	Enabled     bool      `json:"enabled"`
	SortOrder   int       `json:"sort_order"`
	CreatedAt   time.Time `json:"created_at"`
	UpdatedAt   time.Time `json:"updated_at"`
}

type desktopAIModelPayload struct {
	ID          uuid.UUID `json:"id"`
	DisplayName string    `json:"display_name"`
	Provider    string    `json:"provider"`
	Model       string    `json:"model"`
}

func adminAIModel(model model.AIModel) adminAIModelPayload {
	return adminAIModelPayload{
		ID: model.ID, DisplayName: model.DisplayName, Provider: model.Provider,
		Model: model.ModelName, EndpointURL: model.EndpointURL,
		HasAPIKey: model.APIKeyCiphertext != "", Enabled: model.Enabled,
		SortOrder: model.SortOrder, CreatedAt: model.CreatedAt,
		UpdatedAt: model.UpdatedAt,
	}
}

func validateAIModel(input *aiModelWritePayload, production bool, requireKey bool) map[string]string {
	input.DisplayName = strings.TrimSpace(input.DisplayName)
	input.Provider = strings.ToLower(strings.TrimSpace(input.Provider))
	input.Model = strings.TrimSpace(input.Model)
	input.EndpointURL = strings.TrimSpace(input.EndpointURL)
	input.APIKey = strings.TrimSpace(input.APIKey)
	errorsByField := map[string]string{}
	if input.DisplayName == "" || len([]rune(input.DisplayName)) > 80 {
		errorsByField["display_name"] = "Use a name between 1 and 80 characters."
	}
	if input.Provider != "openai" && input.Provider != "anthropic" {
		errorsByField["provider"] = "Choose OpenAI-compatible or Anthropic-compatible."
	}
	if input.Model == "" || len(input.Model) > 200 {
		errorsByField["model"] = "Enter the model identifier used by the provider."
	}
	parsed, err := url.Parse(input.EndpointURL)
	if err != nil || parsed.Host == "" || (parsed.Scheme != "https" && parsed.Scheme != "http") ||
		parsed.User != nil || parsed.Fragment != "" {
		errorsByField["endpoint_url"] = "Enter a complete HTTP or HTTPS endpoint URL without credentials or a fragment."
	} else if production && parsed.Scheme != "https" {
		errorsByField["endpoint_url"] = "Production model endpoints must use HTTPS."
	}
	if requireKey && input.APIKey == "" {
		errorsByField["api_key"] = "Enter the provider API key."
	}
	if len(input.APIKey) > 16_384 {
		errorsByField["api_key"] = "The API key is too long."
	}
	return errorsByField
}

// resolveAIEndpoint accepts the value users naturally get from provider
// documentation: either an API base URL or the final chat endpoint. The wire
// format in this application is Chat Completions for OpenAI-compatible
// providers and Messages for Anthropic-compatible providers.
func resolveAIEndpoint(rawURL, provider string) (string, error) {
	parsed, err := url.Parse(strings.TrimSpace(rawURL))
	if err != nil || parsed.Host == "" || (parsed.Scheme != "https" && parsed.Scheme != "http") ||
		parsed.User != nil || parsed.Fragment != "" {
		return "", errors.New("invalid AI endpoint URL")
	}

	path := strings.TrimRight(parsed.Path, "/")
	switch provider {
	case "openai":
		if !strings.HasSuffix(path, "/chat/completions") {
			if path == "" {
				path = "/v1"
			}
			path += "/chat/completions"
		}
	case "anthropic":
		if !strings.HasSuffix(path, "/messages") {
			if path == "" {
				path = "/v1"
			}
			path += "/messages"
		}
	default:
		return "", errors.New("unsupported AI provider")
	}
	parsed.Path = path
	parsed.RawPath = ""
	return parsed.String(), nil
}

func (s *Server) desktopAIModels(w http.ResponseWriter, r *http.Request) {
	var stored []model.AIModel
	if err := s.DB.Where("enabled = ?", true).
		Order("sort_order ASC, display_name ASC, id ASC").Find(&stored).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "ai_models_unavailable", "AI models could not be loaded.", nil)
		return
	}
	models := make([]desktopAIModelPayload, 0, len(stored))
	for _, item := range stored {
		models = append(models, desktopAIModelPayload{
			ID: item.ID, DisplayName: item.DisplayName, Provider: item.Provider,
			Model: item.ModelName,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"models": models})
}

func (s *Server) adminAIModels(w http.ResponseWriter, r *http.Request) {
	var stored []model.AIModel
	if err := s.DB.Order("sort_order ASC, display_name ASC, id ASC").Find(&stored).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "ai_models_unavailable", "AI models could not be loaded.", nil)
		return
	}
	models := make([]adminAIModelPayload, 0, len(stored))
	for _, item := range stored {
		models = append(models, adminAIModel(item))
	}
	writeJSON(w, http.StatusOK, map[string]any{"models": models})
}

func (s *Server) adminCreateAIModel(w http.ResponseWriter, r *http.Request) {
	var input aiModelWritePayload
	if !decodeJSON(w, r, &input) {
		return
	}
	if fields := validateAIModel(&input, s.Config.Environment == "production", true); len(fields) != 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "ai_model_invalid", "Check the model connection settings.", fields)
		return
	}
	var count int64
	if err := s.DB.Model(&model.AIModel{}).Count(&count).Error; err != nil {
		writeError(w, r, http.StatusInternalServerError, "ai_models_unavailable", "AI models could not be loaded.", nil)
		return
	}
	if count >= maxAIModels {
		writeError(w, r, http.StatusUnprocessableEntity, "ai_model_limit", "The server already has the maximum number of AI models.", nil)
		return
	}
	ciphertext, err := s.encryptAISecret(input.APIKey)
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "ai_model_not_saved", "The provider key could not be protected.", nil)
		return
	}
	now := time.Now().UTC()
	admin := adminFrom(r).ID
	enabled := true
	if input.Enabled != nil {
		enabled = *input.Enabled
	}
	stored := model.AIModel{
		ID: uuid.New(), DisplayName: input.DisplayName, Provider: input.Provider,
		ModelName: input.Model, EndpointURL: input.EndpointURL,
		APIKeyCiphertext: ciphertext, Enabled: enabled, SortOrder: input.SortOrder,
		UpdatedBy: &admin, CreatedAt: now, UpdatedAt: now,
	}
	if err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Create(&stored).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "ai_model_create", "ai_model", stored.ID,
			map[string]any{"name": stored.DisplayName, "provider": stored.Provider})
	}); err != nil {
		writeError(w, r, http.StatusInternalServerError, "ai_model_not_saved", "The AI model could not be saved.", nil)
		return
	}
	writeJSON(w, http.StatusCreated, adminAIModel(stored))
}

func (s *Server) loadAIModel(w http.ResponseWriter, r *http.Request) (model.AIModel, bool) {
	id, ok := parseUUIDParam(w, r, "modelID")
	if !ok {
		return model.AIModel{}, false
	}
	var stored model.AIModel
	err := s.DB.First(&stored, "id = ?", id).Error
	if errors.Is(err, gorm.ErrRecordNotFound) {
		writeError(w, r, http.StatusNotFound, "ai_model_not_found", "There is no such AI model.", nil)
		return model.AIModel{}, false
	}
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "ai_models_unavailable", "AI models could not be loaded.", nil)
		return model.AIModel{}, false
	}
	return stored, true
}

func (s *Server) adminUpdateAIModel(w http.ResponseWriter, r *http.Request) {
	stored, ok := s.loadAIModel(w, r)
	if !ok {
		return
	}
	var input aiModelWritePayload
	if !decodeJSON(w, r, &input) {
		return
	}
	if fields := validateAIModel(&input, s.Config.Environment == "production", false); len(fields) != 0 {
		writeError(w, r, http.StatusUnprocessableEntity, "ai_model_invalid", "Check the model connection settings.", fields)
		return
	}
	updates := map[string]any{
		"display_name": input.DisplayName, "provider": input.Provider,
		"model_name": input.Model, "endpoint_url": input.EndpointURL,
		"sort_order": input.SortOrder, "updated_by": adminFrom(r).ID,
		"updated_at": time.Now().UTC(),
	}
	if input.Enabled != nil {
		updates["enabled"] = *input.Enabled
	}
	if input.APIKey != "" {
		ciphertext, err := s.encryptAISecret(input.APIKey)
		if err != nil {
			writeError(w, r, http.StatusInternalServerError, "ai_model_not_saved", "The provider key could not be protected.", nil)
			return
		}
		updates["api_key_ciphertext"] = ciphertext
	}
	if err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Model(&model.AIModel{}).Where("id = ?", stored.ID).Updates(updates).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "ai_model_update", "ai_model", stored.ID,
			map[string]any{"name": input.DisplayName, "provider": input.Provider, "key_replaced": input.APIKey != ""})
	}); err != nil {
		writeError(w, r, http.StatusInternalServerError, "ai_model_not_saved", "The AI model could not be saved.", nil)
		return
	}
	if err := s.DB.First(&stored, "id = ?", stored.ID).Error; err != nil {
		writeJSON(w, http.StatusOK, map[string]any{"id": stored.ID})
		return
	}
	writeJSON(w, http.StatusOK, adminAIModel(stored))
}

func (s *Server) adminDeleteAIModel(w http.ResponseWriter, r *http.Request) {
	stored, ok := s.loadAIModel(w, r)
	if !ok {
		return
	}
	if err := s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Delete(&model.AIModel{}, "id = ?", stored.ID).Error; err != nil {
			return err
		}
		return s.audit(tx, r, "ai_model_delete", "ai_model", stored.ID,
			map[string]any{"name": stored.DisplayName, "provider": stored.Provider})
	}); err != nil {
		writeError(w, r, http.StatusInternalServerError, "ai_model_not_deleted", "The AI model could not be deleted.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"deleted": stored.ID})
}
