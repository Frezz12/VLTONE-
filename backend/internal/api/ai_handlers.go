package api

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"
	"time"

	"gorm.io/datatypes"

	"vltstudio/backend/internal/quota"
)

const maxAIRequestBody = 4 << 20

func (s *Server) aiProxy(w http.ResponseWriter, r *http.Request) {
	if !s.Config.AIEnabled {
		writeError(w, r, http.StatusServiceUnavailable, "ai_disabled", "AI is temporarily disabled.", nil)
		return
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxAIRequestBody))
	if err != nil {
		writeError(w, r, http.StatusBadRequest, "ai_request_invalid", "AI request is too large or invalid.", nil)
		return
	}
	var request map[string]any
	if json.Unmarshal(body, &request) != nil {
		writeError(w, r, http.StatusBadRequest, "ai_request_invalid", "AI request must be valid JSON.", nil)
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
	provider := connection.Provider
	modelName := connection.ModelName
	request["model"] = modelName
	key, err := s.decryptAISecret(connection.APIKeyCiphertext)
	if err != nil {
		writeError(w, r, http.StatusServiceUnavailable, "ai_provider_credential_invalid", "The stored AI provider key cannot be read. An administrator must enter the key again.", nil)
		return
	}
	if key == "" {
		writeError(w, r, http.StatusServiceUnavailable, "ai_provider_unavailable", "This AI provider has no API key configured.", nil)
		return
	}
	endpoint, err := resolveAIEndpoint(connection.EndpointURL, provider)
	if err != nil {
		writeError(w, r, http.StatusServiceUnavailable, "ai_provider_endpoint_invalid", "This AI provider has an invalid endpoint URL.", nil)
		return
	}
	maxOutput := int64(numberValue(request["max_tokens"]))
	if provider == "openai" {
		if value := int64(numberValue(request["max_completion_tokens"])); value > 0 {
			maxOutput = value
		}
		request["stream_options"] = map[string]any{"include_usage": true}
	}
	if maxOutput <= 0 || maxOutput > 1_000_000 {
		writeError(w, r, http.StatusUnprocessableEntity, "ai_request_invalid", "A valid output-token limit is required.", map[string]string{"max_tokens": "Use a value between 1 and 1000000."})
		return
	}
	upstreamBody, _ := json.Marshal(request)
	// One token per UTF-8 byte is deliberately conservative across languages
	// and provider tokenizers. The unused part is released on settlement.
	estimatedInput := int64(len(upstreamBody))
	reserved := estimatedInput + maxOutput
	reservation, err := s.Quota.Reserve(userFrom(r).ID, provider, modelName, reserved, time.Now().UTC())
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

	actual, completed := int64(0), false
	settled := false
	settle := func(amount int64, reason string) {
		if settled {
			return
		}
		settled = true
		metadata, _ := json.Marshal(map[string]any{"provider": provider, "model": modelName, "settlement": reason})
		if err := s.Quota.Settle(reservation.ID, amount, datatypes.JSON(metadata), time.Now().UTC()); err != nil {
			// The reservation deliberately remains held if settlement fails. This fails closed.
			return
		}
	}
	defer func() {
		if !settled {
			if completed && actual > 0 {
				settle(actual, "provider_usage")
			} else {
				settle(reserved, "conservative_no_usage")
			}
		}
	}()

	upstream, err := http.NewRequestWithContext(r.Context(), http.MethodPost, endpoint, bytes.NewReader(upstreamBody))
	if err != nil {
		writeError(w, r, http.StatusInternalServerError, "ai_upstream_failed", "AI request could not be created.", nil)
		return
	}
	upstream.Header.Set("Content-Type", "application/json")
	if provider == "openai" {
		upstream.Header.Set("Authorization", "Bearer "+key)
	} else {
		upstream.Header.Set("x-api-key", key)
		upstream.Header.Set("anthropic-version", "2023-06-01")
	}
	response, err := (&http.Client{Timeout: 4 * time.Minute}).Do(upstream)
	if err != nil {
		if errors.Is(err, context.Canceled) {
			return
		}
		settle(reserved, "upstream_transport_failure")
		writeError(w, r, http.StatusBadGateway, "ai_upstream_failed", "AI provider could not be reached.", nil)
		return
	}
	defer response.Body.Close()
	if response.StatusCode >= 400 {
		settle(0, "provider_rejected")
		message := "AI provider rejected the request."
		payload, readErr := io.ReadAll(io.LimitReader(response.Body, 64<<10))
		if readErr == nil {
			if detail := aiProviderErrorMessage(payload); detail != "" {
				message = "AI provider rejected the request: " + detail
			}
		}
		writeError(w, r, http.StatusBadGateway, "ai_provider_error", message, nil)
		return
	}
	contentType := response.Header.Get("Content-Type")
	w.Header().Set("Content-Type", contentType)
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(response.StatusCode)
	if strings.Contains(contentType, "text/event-stream") {
		actual, completed = proxySSE(w, response.Body, provider)
	} else {
		payload, readErr := io.ReadAll(io.LimitReader(response.Body, 32<<20))
		if readErr == nil {
			actual = extractUsage(payload, provider)
			_, _ = w.Write(payload)
			completed = true
		}
	}
	if completed && actual > 0 {
		settle(actual, "provider_usage")
	}
}

func aiProviderErrorMessage(payload []byte) string {
	var body map[string]any
	if json.Unmarshal(payload, &body) != nil {
		return ""
	}
	if providerError, ok := body["error"].(map[string]any); ok {
		if message, ok := providerError["message"].(string); ok {
			return strings.TrimSpace(message)
		}
	}
	if message, ok := body["message"].(string); ok {
		return strings.TrimSpace(message)
	}
	return ""
}

func proxySSE(w http.ResponseWriter, source io.Reader, provider string) (int64, bool) {
	reader := bufio.NewReaderSize(source, 64<<10)
	flusher, _ := w.(http.Flusher)
	usage := usageAccumulator{provider: provider}
	for {
		line, err := reader.ReadBytes('\n')
		if len(line) != 0 {
			if _, writeErr := w.Write(line); writeErr != nil {
				return usage.total(), false
			}
			if flusher != nil {
				flusher.Flush()
			}
			trimmed := bytes.TrimSpace(line)
			if bytes.HasPrefix(trimmed, []byte("data:")) {
				data := bytes.TrimSpace(bytes.TrimPrefix(trimmed, []byte("data:")))
				if !bytes.Equal(data, []byte("[DONE]")) {
					usage.add(data)
				}
			}
		}
		if err != nil {
			return usage.total(), errors.Is(err, io.EOF)
		}
	}
}

type usageAccumulator struct {
	provider string
	openAI   int64
	input    int64
	output   int64
	created  int64
	read     int64
}

func (u *usageAccumulator) add(payload []byte) {
	var value map[string]any
	if json.Unmarshal(payload, &value) != nil {
		return
	}
	usage, _ := value["usage"].(map[string]any)
	if usage == nil {
		if message, ok := value["message"].(map[string]any); ok {
			usage, _ = message["usage"].(map[string]any)
		}
	}
	if usage == nil {
		return
	}
	if u.provider == "openai" {
		u.openAI = max64(u.openAI, int64(numberValue(usage["total_tokens"])))
		return
	}
	u.input = max64(u.input, int64(numberValue(usage["input_tokens"])))
	u.output = max64(u.output, int64(numberValue(usage["output_tokens"])))
	u.created = max64(u.created, int64(numberValue(usage["cache_creation_input_tokens"])))
	u.read = max64(u.read, int64(numberValue(usage["cache_read_input_tokens"])))
}

func (u usageAccumulator) total() int64 {
	if u.provider == "openai" {
		return u.openAI
	}
	return u.input + u.output + u.created + u.read
}

func max64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

func extractUsage(payload []byte, provider string) int64 {
	var value map[string]any
	if json.Unmarshal(payload, &value) != nil {
		return 0
	}
	usage, _ := value["usage"].(map[string]any)
	if provider == "openai" {
		return int64(numberValue(usage["total_tokens"]))
	}
	return int64(numberValue(usage["input_tokens"]) + numberValue(usage["output_tokens"]) +
		numberValue(usage["cache_creation_input_tokens"]) + numberValue(usage["cache_read_input_tokens"]))
}

func numberValue(value any) float64 {
	switch number := value.(type) {
	case float64:
		return number
	case json.Number:
		parsed, _ := number.Float64()
		return parsed
	default:
		return 0
	}
}
