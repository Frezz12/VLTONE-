package api

import (
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"

	"vltstudio/backend/internal/quota"
)

const maxAIOutputTokens = 1_000_000

type aiLeaseRequest struct {
	InputBytes      int64 `json:"input_bytes"`
	MaxOutputTokens int64 `json:"max_output_tokens"`
}

type aiLeasePayload struct {
	ReservationID  uuid.UUID `json:"reservation_id"`
	ReservedTokens int64     `json:"reserved_tokens"`
	Provider       string    `json:"provider"`
	Model          string    `json:"model"`
	EndpointURL    string    `json:"endpoint_url"`
	APIKey         string    `json:"api_key"`
}

type aiSettlementRequest struct {
	ActualTokens int64  `json:"actual_tokens"`
	Outcome      string `json:"outcome"`
}

// leaseAIModel authorizes one provider request. The secret is intentionally
// returned only here, after the current model state and quota have both been
// checked; list responses and local settings never contain it.
func (s *Server) leaseAIModel(w http.ResponseWriter, r *http.Request) {
	var input aiLeaseRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.InputBytes <= 0 || input.InputBytes > maxAIRequestBody ||
		input.MaxOutputTokens <= 0 || input.MaxOutputTokens > maxAIOutputTokens {
		writeError(w, r, http.StatusUnprocessableEntity, "ai_request_invalid",
			"Valid request and output-token sizes are required.", map[string]string{
				"input_bytes":       "Use a value between 1 and 4194304.",
				"max_output_tokens": "Use a value between 1 and 1000000.",
			})
		return
	}

	connection, ok := s.loadAIModel(w, r)
	if !ok {
		return
	}
	if !connection.Enabled {
		writeError(w, r, http.StatusNotFound, "ai_model_not_found", "This AI model is not available.", nil)
		return
	}
	key, err := s.decryptAISecret(connection.APIKeyCiphertext)
	if err != nil {
		writeError(w, r, http.StatusServiceUnavailable, "ai_provider_credential_invalid", "The stored AI provider key cannot be read. An administrator must enter the key again.", nil)
		return
	}
	if strings.TrimSpace(key) == "" {
		writeError(w, r, http.StatusServiceUnavailable, "ai_provider_unavailable", "This AI provider has no API key configured.", nil)
		return
	}
	endpoint, err := resolveAIEndpoint(connection.EndpointURL, connection.Provider)
	if err != nil {
		writeError(w, r, http.StatusServiceUnavailable, "ai_provider_endpoint_invalid", "This AI provider has an invalid endpoint URL.", nil)
		return
	}

	reserved := input.InputBytes + input.MaxOutputTokens
	reservation, err := s.Quota.Reserve(userFrom(r).ID, connection.Provider, connection.ModelName, reserved, time.Now().UTC())
	if errors.Is(err, quota.ErrExhausted) {
		writeError(w, r, http.StatusPaymentRequired, "ai_quota_exhausted", "Your monthly AI quota is exhausted. The rest of VLT Studio remains available.", nil)
		return
	}
	if errors.Is(err, quota.ErrGlobalExhausted) {
		writeError(w, r, http.StatusServiceUnavailable, "ai_global_budget_exhausted", "The monthly AI service budget is exhausted.", nil)
		return
	}
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "ai_quota_unavailable", "AI quota could not be reserved.", nil)
		return
	}

	w.Header().Set("Cache-Control", "no-store, max-age=0")
	writeJSON(w, http.StatusCreated, aiLeasePayload{
		ReservationID:  reservation.ID,
		ReservedTokens: reservation.Reserved,
		Provider:       connection.Provider,
		Model:          connection.ModelName,
		EndpointURL:    endpoint,
		APIKey:         key,
	})
}

func (s *Server) settleAIReservation(w http.ResponseWriter, r *http.Request) {
	reservationID, ok := parseUUIDParam(w, r, "reservationID")
	if !ok {
		return
	}
	var input aiSettlementRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.ActualTokens < 0 || !validAISettlementOutcome(input.Outcome) {
		writeError(w, r, http.StatusUnprocessableEntity, "ai_settlement_invalid", "Valid token usage and outcome are required.", nil)
		return
	}
	metadata, _ := json.Marshal(map[string]any{"settlement": input.Outcome, "source": "desktop_direct"})
	userID := userFrom(r).ID
	if err := s.Quota.SettleForUser(userID, reservationID, input.ActualTokens, datatypes.JSON(metadata), time.Now().UTC()); err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			writeError(w, r, http.StatusNotFound, "ai_reservation_not_found", "This AI reservation is not available.", nil)
			return
		}
		writeError(w, r, http.StatusInternalServerError, "ai_quota_unavailable", "AI usage could not be settled.", nil)
		return
	}
	cycle, err := s.Quota.Current(userID, time.Now().UTC())
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "quota_unavailable", "Quota is unavailable.", nil)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"quota": quotaResponse(cycle)})
}

func validAISettlementOutcome(value string) bool {
	switch value {
	case "provider_usage", "provider_rejected", "transport_failure", "cancelled", "conservative_no_usage":
		return true
	default:
		return false
	}
}
