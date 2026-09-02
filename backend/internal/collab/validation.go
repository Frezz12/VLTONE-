package collab

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"regexp"
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
)

const (
	CollaborationProtocol             = "vlt-collab-v1"
	CollaborationProjectFormatVersion = 6
	CollaborationCommandSchemaVersion = 1
	MaxOperationPayloadBytes          = 1 << 20
	MaxOperationPreconditions         = 1024
	MaxOperationTouchedFields         = 8192
	DefaultBootstrapLimit             = 500
	MaxBootstrapLimit                 = 2000
	DefaultLeaseTTL                   = 30 * time.Second
	MinLeaseTTL                       = 5 * time.Second
	MaxLeaseTTL                       = 2 * time.Minute
)

var operationKindPattern = regexp.MustCompile(`^(batch|[a-z][A-Za-z0-9_]*(\.[a-z][A-Za-z0-9_]*)+)$`)

var (
	ErrNotFound            = errors.New("collaboration resource not found")
	ErrValidation          = errors.New("collaboration validation failed")
	ErrVersionMismatch     = errors.New("collaboration client version mismatch")
	ErrForbidden           = errors.New("collaboration action forbidden")
	ErrConflict            = errors.New("collaboration conflict")
	ErrProjectInactive     = errors.New("cloud project is not active")
	ErrOperationIDReuse    = errors.New("operation id was reused with different content")
	ErrBaseSeqAhead        = errors.New("operation base sequence is ahead of the project")
	ErrBaseSeqMismatch     = errors.New("recording commit base sequence does not match the project head")
	ErrLiveSessionRequired = errors.New("an active collaboration session membership is required")
	ErrEntityDeleted       = errors.New("collaboration entity is deleted")
	ErrSessionEnded        = errors.New("collaboration session has ended")
	ErrSessionFull         = errors.New("collaboration session is full")
	ErrLeaseHeld           = errors.New("track lease is held by another participant")
	ErrLeaseRequired       = errors.New("an active track lease held by this participant is required")
	ErrLeaseExpired        = errors.New("track lease has expired")
	ErrInviteExpired       = errors.New("project invitation has expired")
	ErrInviteUsed          = errors.New("project invitation is no longer available")
)

type ValidationError struct{ Message string }

func (e *ValidationError) Error() string { return e.Message }
func (e *ValidationError) Unwrap() error { return ErrValidation }

func invalidf(format string, values ...any) error {
	return &ValidationError{Message: fmt.Sprintf(format, values...)}
}

type CompatibilityError struct{ Message string }

func (e *CompatibilityError) Error() string { return e.Message }
func (e *CompatibilityError) Unwrap() error { return ErrVersionMismatch }

func incompatiblef(format string, values ...any) error {
	return &CompatibilityError{Message: fmt.Sprintf(format, values...)}
}

type Permission string

const (
	PermissionView          Permission = "view"
	PermissionEdit          Permission = "edit"
	PermissionManageProject Permission = "manage_project"
	PermissionManageMembers Permission = "manage_members"
	PermissionHostSession   Permission = "host_session"
	PermissionJoinSession   Permission = "join_session"
	PermissionAcquireLease  Permission = "acquire_lease"
)

func RoleAllows(role string, permission Permission) bool {
	switch role {
	case "owner":
		return true
	case "editor":
		return permission == PermissionView || permission == PermissionEdit ||
			permission == PermissionHostSession || permission == PermissionJoinSession ||
			permission == PermissionAcquireLease
	case "viewer":
		return permission == PermissionView || permission == PermissionJoinSession
	default:
		return false
	}
}

type FieldPrecondition struct {
	Kind        string    `json:"kind"`
	FieldKey    string    `json:"fieldKey"`
	OperationID uuid.UUID `json:"operationId"`
}

type AppendOperationInput struct {
	ProjectID      uuid.UUID
	ActorUserID    uuid.UUID
	ActorDeviceID  uuid.UUID
	ActorSessionID uuid.UUID
	OpID           uuid.UUID
	TransactionID  *uuid.UUID
	Kind           string
	SchemaVersion  int
	BaseSeq        int64
	Payload        json.RawMessage
	Preconditions  []FieldPrecondition
	// TouchedFields is the camelCase wire sidecar. The server derives the
	// canonical set from kind/payload and rejects any mismatch before append.
	TouchedFields []string
}

type normalizedOperation struct {
	AppendOperationInput
	Payload        json.RawMessage
	Preconditions  []FieldPrecondition
	TouchedFields  []string
	LifecycleSteps []lifecycleStep
	LeasePolicy    commandLeasePolicy
	RequestHash    string
}

type operationHashEnvelope struct {
	OpID          uuid.UUID           `json:"opId"`
	TransactionID *uuid.UUID          `json:"transactionId,omitempty"`
	Kind          string              `json:"kind"`
	SchemaVersion int                 `json:"schemaVersion"`
	BaseSeq       int64               `json:"baseServerSeq"`
	Payload       json.RawMessage     `json:"payload"`
	Preconditions []FieldPrecondition `json:"preconditions"`
	TouchedFields []string            `json:"touchedFields"`
}

func normalizeOperation(input AppendOperationInput) (normalizedOperation, error) {
	if input.ProjectID == uuid.Nil || input.ActorUserID == uuid.Nil ||
		input.ActorDeviceID == uuid.Nil || input.ActorSessionID == uuid.Nil ||
		input.OpID == uuid.Nil {
		return normalizedOperation{}, invalidf("project, actor, device, session and operation identifiers are required")
	}
	if input.TransactionID != nil && *input.TransactionID == uuid.Nil {
		return normalizedOperation{}, invalidf("transaction identifier must be a UUID when present")
	}
	input.Kind = strings.TrimSpace(input.Kind)
	if len(input.Kind) > 100 || !operationKindPattern.MatchString(input.Kind) {
		return normalizedOperation{}, invalidf("operation kind must be batch or a dotted lower-camel identifier")
	}
	if input.SchemaVersion != CollaborationCommandSchemaVersion {
		return normalizedOperation{}, invalidf("command schema version is unsupported")
	}
	if input.BaseSeq < 0 {
		return normalizedOperation{}, invalidf("base sequence cannot be negative")
	}
	if len(input.Payload) == 0 || len(input.Payload) > MaxOperationPayloadBytes {
		return normalizedOperation{}, invalidf("operation payload must contain at most %d bytes", MaxOperationPayloadBytes)
	}
	var payload map[string]any
	decoder := json.NewDecoder(bytes.NewReader(input.Payload))
	decoder.UseNumber()
	if err := decoder.Decode(&payload); err != nil || payload == nil {
		return normalizedOperation{}, invalidf("operation payload must be a JSON object")
	}
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		return normalizedOperation{}, invalidf("operation payload must contain one JSON object")
	}
	canonicalPayload, err := json.Marshal(payload)
	if err != nil {
		return normalizedOperation{}, fmt.Errorf("canonicalize operation payload: %w", err)
	}
	expectedFields, nestedPreconditions, err := deriveCommandMetadata(input.Kind, canonicalPayload, true)
	if err != nil {
		return normalizedOperation{}, err
	}
	lifecycleSteps, err := deriveLifecycleSteps(input.Kind, canonicalPayload, true)
	if err != nil {
		return normalizedOperation{}, err
	}
	leasePolicy, err := deriveCommandLeasePolicy(input.Kind, canonicalPayload, true)
	if err != nil {
		return normalizedOperation{}, err
	}
	if len(input.Preconditions)+len(nestedPreconditions) > MaxOperationPreconditions {
		return normalizedOperation{}, invalidf("operation contains too many preconditions")
	}
	if len(input.TouchedFields) > MaxOperationTouchedFields || len(expectedFields) > MaxOperationTouchedFields {
		return normalizedOperation{}, invalidf("operation touches too many fields")
	}
	preconditions := make([]FieldPrecondition, 0, len(input.Preconditions)+len(nestedPreconditions))
	preconditions = append(preconditions, input.Preconditions...)
	preconditions = append(preconditions, nestedPreconditions...)
	seenPreconditions := make(map[string]uuid.UUID, len(preconditions))
	uniquePreconditions := preconditions[:0]
	for index := range preconditions {
		if preconditions[index].Kind != "fieldWriterIs" || preconditions[index].OperationID == uuid.Nil {
			return normalizedOperation{}, invalidf("field precondition must be fieldWriterIs with an operationId")
		}
		preconditions[index].FieldKey = strings.TrimSpace(preconditions[index].FieldKey)
		if err := validateFieldKey(preconditions[index].FieldKey); err != nil {
			return normalizedOperation{}, err
		}
		if operationID, exists := seenPreconditions[preconditions[index].FieldKey]; exists {
			if operationID != preconditions[index].OperationID {
				return normalizedOperation{}, invalidf("conflicting field precondition %q", preconditions[index].FieldKey)
			}
			continue
		}
		seenPreconditions[preconditions[index].FieldKey] = preconditions[index].OperationID
		uniquePreconditions = append(uniquePreconditions, preconditions[index])
	}
	preconditions = uniquePreconditions
	sort.Slice(preconditions, func(i, j int) bool { return preconditions[i].FieldKey < preconditions[j].FieldKey })
	updates := append([]string(nil), input.TouchedFields...)
	seenUpdates := make(map[string]bool, len(updates))
	for index := range updates {
		updates[index] = strings.TrimSpace(updates[index])
		if err := validateFieldKey(updates[index]); err != nil {
			return normalizedOperation{}, err
		}
		if seenUpdates[updates[index]] {
			return normalizedOperation{}, invalidf("duplicate field update %q", updates[index])
		}
		seenUpdates[updates[index]] = true
	}
	sort.Strings(updates)
	if !equalStrings(updates, expectedFields) {
		return normalizedOperation{}, invalidf("touchedFields do not match the command payload")
	}
	envelope := operationHashEnvelope{
		OpID: input.OpID, TransactionID: input.TransactionID, Kind: input.Kind, SchemaVersion: input.SchemaVersion,
		BaseSeq: input.BaseSeq, Payload: canonicalPayload, Preconditions: preconditions,
		TouchedFields: updates,
	}
	body, err := json.Marshal(envelope)
	if err != nil {
		return normalizedOperation{}, err
	}
	digest := sha256.Sum256(body)
	input.Payload = canonicalPayload
	input.Preconditions = preconditions
	input.TouchedFields = updates
	return normalizedOperation{
		AppendOperationInput: input, Payload: canonicalPayload, Preconditions: preconditions,
		TouchedFields: updates, LifecycleSteps: lifecycleSteps, LeasePolicy: leasePolicy,
		RequestHash: hex.EncodeToString(digest[:]),
	}, nil
}

func equalStrings(left, right []string) bool {
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

func validateFieldKey(value string) error {
	if value == "" || !utf8.ValidString(value) || len([]rune(value)) > 512 || strings.ContainsAny(value, "\x00\r\n") {
		return invalidf("field key must contain between 1 and 512 safe characters")
	}
	return nil
}

func ParseOperationUUID(value string, optional bool) (*uuid.UUID, error) {
	if value == "" && optional {
		return nil, nil
	}
	parsed, err := uuid.Parse(value)
	if err != nil || parsed == uuid.Nil || !isWireUUID(value) {
		return nil, invalidf("operation identifier must be a UUID")
	}
	return &parsed, nil
}

func normalizeBootstrapLimit(limit int) int {
	if limit <= 0 {
		return DefaultBootstrapLimit
	}
	if limit > MaxBootstrapLimit {
		return MaxBootstrapLimit
	}
	return limit
}

func normalizeLeaseTTL(ttl time.Duration) (time.Duration, error) {
	if ttl == 0 {
		return DefaultLeaseTTL, nil
	}
	if ttl < MinLeaseTTL || ttl > MaxLeaseTTL {
		return 0, invalidf("lease TTL must be between %s and %s", MinLeaseTTL, MaxLeaseTTL)
	}
	return ttl, nil
}

func ValidProjectRole(role string) bool {
	return role == "editor" || role == "viewer"
}

func ValidSessionMode(mode string) bool {
	return mode == "independent" || mode == "follow_host" || mode == "synchronized"
}
