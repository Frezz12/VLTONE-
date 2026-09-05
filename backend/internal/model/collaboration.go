package model

import (
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
)

const (
	ProjectUploading = "uploading"
	ProjectActive    = "active"
	ProjectReadOnly  = "read_only"
	ProjectConflict  = "conflict"
	ProjectArchived  = "archived"

	ProjectRoleOwner  = "owner"
	ProjectRoleEditor = "editor"
	ProjectRoleViewer = "viewer"

	ProjectSessionStarting = "starting"
	ProjectSessionActive   = "active"
	ProjectSessionEnding   = "ending"
	ProjectSessionEnded    = "ended"

	SessionReadinessReady   = "ready"
	SessionReadinessBlocked = "blocked"
	SessionReadinessViewer  = "viewer"

	SnapshotReasonAutosave    = "autosave"
	SnapshotReasonSessionEnd  = "session_end"
	SnapshotRequestPending    = "pending"
	SnapshotRequestCompleted  = "completed"
	SnapshotRequestSuperseded = "superseded"

	SessionModeIndependent  = "independent"
	SessionModeFollowHost   = "follow_host"
	SessionModeSynchronized = "synchronized"

	TrackLeaseRecord = "record"
)

type CloudProject struct {
	ID                uuid.UUID  `gorm:"type:uuid;primaryKey" json:"id"`
	OwnerUserID       uuid.UUID  `gorm:"type:uuid;index;not null" json:"owner_user_id"`
	Title             string     `gorm:"not null" json:"title"`
	Status            string     `gorm:"not null;default:uploading" json:"status"`
	FormatVersion     int        `gorm:"not null" json:"format_version"`
	EngineVersion     string     `gorm:"not null;default:''" json:"engine_version"`
	MinimumAppVersion string     `gorm:"not null;default:''" json:"minimum_app_version"`
	HeadSeq           int64      `gorm:"not null;default:0" json:"head_seq"`
	SnapshotSeq       int64      `gorm:"not null;default:0" json:"snapshot_seq"`
	CreatedAt         time.Time  `json:"created_at"`
	UpdatedAt         time.Time  `json:"updated_at"`
	ArchivedAt        *time.Time `json:"archived_at"`
}

type ProjectMember struct {
	ProjectID  uuid.UUID  `gorm:"type:uuid;primaryKey" json:"project_id"`
	UserID     uuid.UUID  `gorm:"type:uuid;primaryKey" json:"user_id"`
	Role       string     `gorm:"not null" json:"role"`
	ColorIndex int16      `gorm:"not null;default:0" json:"color_index"`
	InvitedBy  *uuid.UUID `gorm:"type:uuid" json:"invited_by,omitempty"`
	JoinedAt   time.Time  `json:"joined_at"`
	UpdatedAt  time.Time  `json:"updated_at"`
}

type ProjectInvite struct {
	ID             uuid.UUID  `gorm:"type:uuid;primaryKey" json:"id"`
	ProjectID      uuid.UUID  `gorm:"type:uuid;index;not null" json:"project_id"`
	InvitedBy      *uuid.UUID `gorm:"type:uuid" json:"invited_by,omitempty"`
	TargetEmailKey *string    `json:"-"`
	Role           string     `gorm:"not null" json:"role"`
	TokenHash      string     `gorm:"uniqueIndex;not null" json:"-"`
	ExpiresAt      time.Time  `gorm:"index;not null" json:"expires_at"`
	AcceptedBy     *uuid.UUID `gorm:"type:uuid" json:"accepted_by,omitempty"`
	AcceptedAt     *time.Time `json:"accepted_at,omitempty"`
	RevokedAt      *time.Time `json:"revoked_at,omitempty"`
	CreatedAt      time.Time  `json:"created_at"`
	// Peppered HMAC of the short numeric code, never the code itself. The
	// `json:"-"` on this and the two counters below is load-bearing: this
	// struct is serialised straight to the client by ListInvites, and a
	// disclosed lookup value would let anyone confirm a guess offline.
	CodeLookup   *string    `gorm:"uniqueIndex" json:"-"`
	CodeDigits   int16      `gorm:"not null;default:0" json:"code_digits"`
	AttemptCount int        `gorm:"not null;default:0" json:"-"`
	LockedUntil  *time.Time `json:"-"`
}

// InviteAttempt records one redemption attempt, successful or not, so the
// per-account and per-IP windows can be enforced. The client IP is stored only
// as a digest.
type InviteAttempt struct {
	ID          uuid.UUID  `gorm:"type:uuid;primaryKey" json:"-"`
	ActorUserID uuid.UUID  `gorm:"type:uuid;index;not null" json:"-"`
	ActorIPHash string     `gorm:"index;not null" json:"-"`
	InviteID    *uuid.UUID `gorm:"type:uuid" json:"-"`
	Succeeded   bool       `gorm:"not null;default:false" json:"-"`
	AttemptedAt time.Time  `gorm:"index;not null" json:"-"`
}

func (InviteAttempt) TableName() string { return "project_invite_attempts" }

type ProjectOperation struct {
	ProjectID     uuid.UUID      `gorm:"type:uuid;primaryKey" json:"project_id"`
	Seq           int64          `gorm:"primaryKey" json:"serverSeq"`
	OpID          uuid.UUID      `gorm:"type:uuid;not null" json:"opId"`
	TransactionID *uuid.UUID     `gorm:"type:uuid" json:"transactionId,omitempty"`
	ActorUserID   *uuid.UUID     `gorm:"type:uuid" json:"actor_user_id,omitempty"`
	ActorDeviceID *uuid.UUID     `gorm:"type:uuid" json:"actor_device_id,omitempty"`
	Kind          string         `gorm:"not null" json:"kind"`
	SchemaVersion int            `gorm:"not null;default:1" json:"schemaVersion"`
	BaseSeq       int64          `gorm:"not null" json:"baseServerSeq"`
	Payload       datatypes.JSON `gorm:"type:jsonb;not null" json:"payload"`
	Preconditions datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"preconditions"`
	TouchedFields datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"touchedFields"`
	RequestHash   string         `gorm:"not null" json:"-"`
	CreatedAt     time.Time      `json:"created_at"`
}

func (ProjectOperation) TableName() string { return "project_ops" }

type ProjectFieldHead struct {
	ProjectID uuid.UUID `gorm:"type:uuid;primaryKey" json:"project_id"`
	FieldKey  string    `gorm:"primaryKey" json:"field_key"`
	HeadSeq   int64     `gorm:"not null" json:"head_seq"`
	HeadOpID  uuid.UUID `gorm:"type:uuid;not null" json:"headOpId"`
	UpdatedAt time.Time `json:"updated_at"`
}

type Blob struct {
	ID             uuid.UUID  `gorm:"type:uuid;primaryKey" json:"id"`
	SHA256         string     `gorm:"uniqueIndex;not null" json:"sha256"`
	Bytes          int64      `gorm:"not null" json:"bytes"`
	ContentType    string     `gorm:"not null" json:"content_type"`
	Kind           string     `gorm:"not null" json:"kind"`
	ObjectKey      string     `gorm:"uniqueIndex;not null" json:"-"`
	Status         string     `gorm:"not null;default:pending" json:"status"`
	CreatedBy      *uuid.UUID `gorm:"type:uuid" json:"created_by,omitempty"`
	CreatedAt      time.Time  `json:"created_at"`
	VerifiedAt     *time.Time `json:"verified_at,omitempty"`
	UnreferencedAt *time.Time `json:"-"`
}

type ProjectAsset struct {
	ProjectID   uuid.UUID  `gorm:"type:uuid;primaryKey" json:"project_id"`
	AssetID     uuid.UUID  `gorm:"type:uuid;primaryKey" json:"asset_id"`
	BlobID      uuid.UUID  `gorm:"type:uuid;index;not null" json:"blob_id"`
	Kind        string     `gorm:"not null" json:"kind"`
	DisplayName string     `gorm:"not null;default:''" json:"display_name"`
	CreatedBy   *uuid.UUID `gorm:"type:uuid" json:"created_by,omitempty"`
	CreatedAt   time.Time  `json:"created_at"`
}

type ProjectOperationAsset struct {
	ProjectID    uuid.UUID `gorm:"type:uuid;primaryKey" json:"-"`
	OperationSeq int64     `gorm:"primaryKey" json:"-"`
	AssetID      uuid.UUID `gorm:"type:uuid;primaryKey" json:"-"`
}

func (ProjectOperationAsset) TableName() string { return "project_operation_assets" }

type ProjectSnapshotAsset struct {
	SnapshotID uuid.UUID `gorm:"type:uuid;primaryKey" json:"-"`
	ProjectID  uuid.UUID `gorm:"type:uuid;index;not null" json:"-"`
	AssetID    uuid.UUID `gorm:"type:uuid;primaryKey" json:"-"`
}

func (ProjectSnapshotAsset) TableName() string { return "project_snapshot_assets" }

type UploadSession struct {
	ID                    uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	ProjectID             uuid.UUID      `gorm:"type:uuid;index;not null" json:"project_id"`
	BlobID                *uuid.UUID     `gorm:"type:uuid" json:"blob_id,omitempty"`
	AssetID               *uuid.UUID     `gorm:"type:uuid" json:"asset_id,omitempty"`
	CreatedBy             uuid.UUID      `gorm:"type:uuid;not null" json:"created_by"`
	DeviceID              *uuid.UUID     `gorm:"type:uuid" json:"device_id,omitempty"`
	ExpectedSHA256        string         `gorm:"not null" json:"expected_sha256"`
	ExpectedBytes         int64          `gorm:"not null" json:"expected_bytes"`
	Kind                  string         `gorm:"not null" json:"kind"`
	SnapshotSeq           *int64         `json:"snapshot_seq,omitempty"`
	SnapshotSchemaVersion *int           `json:"snapshot_schema_version,omitempty"`
	SnapshotAssetIDs      datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"-"`
	ContentType           string         `gorm:"not null" json:"content_type"`
	DisplayName           string         `gorm:"not null;default:''" json:"display_name"`
	RequestHash           string         `gorm:"not null" json:"-"`
	ProviderUploadID      string         `gorm:"not null;default:''" json:"-"`
	UploadMode            string         `gorm:"not null;default:single" json:"-"`
	MultipartPartSize     *int64         `json:"-"`
	MultipartPartCount    *int           `json:"-"`
	MultipartState        string         `gorm:"not null;default:none" json:"-"`
	MultipartManifest     datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"-"`
	ObjectKey             string         `gorm:"not null" json:"-"`
	Status                string         `gorm:"not null;default:pending" json:"status"`
	ExpiresAt             time.Time      `gorm:"index;not null" json:"expires_at"`
	CreatedAt             time.Time      `json:"created_at"`
	CompletedAt           *time.Time     `json:"completed_at,omitempty"`
	VerificationStartedAt *time.Time     `json:"-"`
}

type ObjectCleanupJob struct {
	ID               uuid.UUID  `gorm:"type:uuid;primaryKey" json:"-"`
	ObjectKey        string     `gorm:"not null" json:"-"`
	ProviderUploadID string     `gorm:"not null;default:''" json:"-"`
	AbortMultipart   bool       `gorm:"not null;default:false" json:"-"`
	DeleteObject     bool       `gorm:"not null" json:"-"`
	Status           string     `gorm:"not null;default:pending" json:"-"`
	AttemptCount     int        `gorm:"not null;default:0" json:"-"`
	RetryAvailableAt time.Time  `gorm:"not null" json:"-"`
	LeaseExpiresAt   *time.Time `json:"-"`
	ClaimToken       *uuid.UUID `gorm:"type:uuid" json:"-"`
	LastError        string     `gorm:"not null;default:''" json:"-"`
	CreatedAt        time.Time  `json:"-"`
	UpdatedAt        time.Time  `json:"-"`
}

func (ObjectCleanupJob) TableName() string { return "object_cleanup_jobs" }

type ProjectSnapshot struct {
	ID            uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	ProjectID     uuid.UUID      `gorm:"type:uuid;index;not null" json:"project_id"`
	Seq           int64          `gorm:"not null" json:"seq"`
	BlobID        uuid.UUID      `gorm:"type:uuid;not null" json:"blob_id"`
	SchemaVersion int            `gorm:"not null" json:"schema_version"`
	AssetIDs      datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"asset_ids"`
	CreatedBy     *uuid.UUID     `gorm:"type:uuid" json:"created_by,omitempty"`
	CreatedAt     time.Time      `json:"created_at"`
}

type ProjectSnapshotRequest struct {
	ID                  uuid.UUID  `gorm:"type:uuid;primaryKey" json:"id"`
	ProjectID           uuid.UUID  `gorm:"type:uuid;index;not null" json:"projectId"`
	SessionID           uuid.UUID  `gorm:"type:uuid;index;not null" json:"sessionId"`
	TargetSeq           int64      `gorm:"not null" json:"targetServerSeq"`
	Reason              string     `gorm:"not null" json:"reason"`
	Status              string     `gorm:"not null" json:"status"`
	AssignedMemberID    *uuid.UUID `gorm:"type:uuid" json:"hostParticipantId,omitempty"`
	AttemptCount        int        `gorm:"not null" json:"attempt"`
	RequestedAt         time.Time  `json:"requestedAt"`
	LastDispatchedAt    *time.Time `json:"lastDispatchedAt,omitempty"`
	NextRetryAt         time.Time  `json:"nextRetryAt"`
	CompletedSnapshotID *uuid.UUID `gorm:"type:uuid" json:"completedSnapshotId,omitempty"`
	CompletedAt         *time.Time `json:"completedAt,omitempty"`
}

func (ProjectSnapshotRequest) TableName() string { return "project_snapshot_requests" }

type ProjectSession struct {
	ID                         uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	ProjectID                  uuid.UUID      `gorm:"type:uuid;index;not null" json:"project_id"`
	CreatedBy                  *uuid.UUID     `gorm:"type:uuid" json:"created_by,omitempty"`
	HostMemberID               *uuid.UUID     `gorm:"type:uuid" json:"host_member_id,omitempty"`
	Mode                       string         `gorm:"not null;default:independent" json:"mode"`
	Status                     string         `gorm:"not null;default:starting" json:"status"`
	Version                    int64          `gorm:"not null;default:1" json:"version"`
	CommandSchemaVersion       int            `gorm:"not null;default:2" json:"command_schema_version"`
	PluginRequirementsRevision int64          `gorm:"not null;default:0" json:"plugin_requirements_revision"`
	PluginRequirements         datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"plugin_requirements"`
	CreatedAt                  time.Time      `json:"created_at"`
	StartedAt                  *time.Time     `json:"started_at,omitempty"`
	UpdatedAt                  time.Time      `json:"updated_at"`
	EndedAt                    *time.Time     `json:"ended_at,omitempty"`
	// Argon2id, and never serialised. Clients learn only PasswordRequired().
	PasswordHash  *string    `json:"-"`
	PasswordSetAt *time.Time `json:"-"`
}

func (ProjectSession) TableName() string { return "project_live_sessions" }

// PasswordRequired is the only thing about the session secret a client is told.
func (s ProjectSession) PasswordRequired() bool { return s.PasswordHash != nil }

type ProjectSessionMember struct {
	ID                uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	SessionID         uuid.UUID      `gorm:"type:uuid;index;not null" json:"session_id"`
	UserID            uuid.UUID      `gorm:"type:uuid;not null" json:"user_id"`
	DeviceID          uuid.UUID      `gorm:"type:uuid;not null" json:"device_id"`
	DesktopSessionID  *uuid.UUID     `gorm:"type:uuid" json:"-"`
	EffectiveRole     string         `gorm:"not null;default:viewer" json:"effective_role"`
	ReadinessStatus   string         `gorm:"not null;default:ready" json:"readiness_status"`
	ReadinessRevision int64          `gorm:"not null;default:0" json:"readiness_revision"`
	PluginReadiness   datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"plugin_readiness"`
	JoinedAt          time.Time      `json:"joined_at"`
	LastSeenAt        time.Time      `json:"last_seen_at"`
	LeftAt            *time.Time     `json:"left_at,omitempty"`
}

type ProjectTrackLease struct {
	ID             uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	ProjectID      uuid.UUID `gorm:"type:uuid;index;not null" json:"project_id"`
	SessionID      uuid.UUID `gorm:"type:uuid;index;not null" json:"session_id"`
	TrackID        uuid.UUID `gorm:"type:uuid;not null" json:"track_id"`
	LeaseKind      string    `gorm:"not null;default:record" json:"lease_kind"`
	HolderMemberID uuid.UUID `gorm:"type:uuid;index;not null" json:"holder_member_id"`
	AcquiredAt     time.Time `json:"acquired_at"`
	RenewedAt      time.Time `json:"renewed_at"`
	ExpiresAt      time.Time `gorm:"index;not null" json:"expires_at"`
}

func (ProjectTrackLease) TableName() string { return "project_leases" }
