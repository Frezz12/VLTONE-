package model

import (
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
)

const (
	UserActive    = "active"
	UserSuspended = "suspended"
	PlanDemo      = "demo"
	BaseDemoLimit = int64(20_000_000)
)

type User struct {
	ID                uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	Email             string    `gorm:"not null" json:"email"`
	EmailKey          string    `gorm:"uniqueIndex;not null" json:"-"`
	Nickname          string    `gorm:"not null" json:"nickname"`
	NicknameKey       string    `gorm:"uniqueIndex;not null" json:"-"`
	PasswordHash      string    `gorm:"not null" json:"-"`
	Locale            string    `gorm:"not null;default:en" json:"locale"`
	Status            string    `gorm:"not null;default:active" json:"status"`
	ConsentVersion    string    `gorm:"not null" json:"consent_version"`
	ConsentAcceptedAt time.Time `gorm:"not null" json:"consent_accepted_at"`
	ConsentIP         string    `gorm:"not null" json:"-"`
	CreatedAt         time.Time `json:"created_at"`
	UpdatedAt         time.Time `json:"updated_at"`
}

type AdminUser struct {
	ID           uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	Email        string    `gorm:"not null" json:"email"`
	EmailKey     string    `gorm:"uniqueIndex;not null" json:"-"`
	Nickname     string    `gorm:"not null" json:"nickname"`
	PasswordHash string    `gorm:"not null" json:"-"`
	Status       string    `gorm:"not null;default:active" json:"status"`
	CreatedAt    time.Time `json:"created_at"`
	UpdatedAt    time.Time `json:"updated_at"`
}

type Plan struct {
	ID                uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	Code              string    `gorm:"uniqueIndex;not null" json:"code"`
	DisplayName       string    `gorm:"not null" json:"display_name"`
	MonthlyTokenLimit int64     `gorm:"not null" json:"monthly_token_limit"`
	DeviceLimit       int       `gorm:"not null" json:"device_limit"`
	AllFeatures       bool      `gorm:"not null" json:"all_features"`
	CreatedAt         time.Time `json:"created_at"`
}

type Subscription struct {
	ID        uuid.UUID  `gorm:"type:uuid;primaryKey" json:"id"`
	UserID    uuid.UUID  `gorm:"type:uuid;uniqueIndex;not null" json:"user_id"`
	PlanID    uuid.UUID  `gorm:"type:uuid;not null" json:"plan_id"`
	Status    string     `gorm:"not null;default:active" json:"status"`
	StartsAt  time.Time  `gorm:"not null" json:"starts_at"`
	EndsAt    *time.Time `json:"ends_at"`
	CreatedAt time.Time  `json:"created_at"`
	UpdatedAt time.Time  `json:"updated_at"`
	Plan      Plan       `json:"plan"`
}

type WebSession struct {
	ID         uuid.UUID `gorm:"type:uuid;primaryKey"`
	UserID     uuid.UUID `gorm:"type:uuid;index;not null"`
	TokenHash  string    `gorm:"uniqueIndex;not null"`
	CSRFToken  string    `gorm:"not null"`
	LastSeenAt time.Time `gorm:"not null"`
	ExpiresAt  time.Time `gorm:"index;not null"`
	CreatedAt  time.Time
	RevokedAt  *time.Time
}

type AdminSession struct {
	ID          uuid.UUID `gorm:"type:uuid;primaryKey"`
	AdminUserID uuid.UUID `gorm:"type:uuid;index;not null"`
	TokenHash   string    `gorm:"uniqueIndex;not null"`
	CSRFToken   string    `gorm:"not null"`
	LastSeenAt  time.Time `gorm:"not null"`
	ExpiresAt   time.Time `gorm:"index;not null"`
	CreatedAt   time.Time
	RevokedAt   *time.Time
}

type Device struct {
	ID          uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	UserID      uuid.UUID      `gorm:"type:uuid;uniqueIndex:device_user_install;not null" json:"user_id"`
	InstallID   string         `gorm:"uniqueIndex:device_user_install;not null" json:"install_id"`
	DisplayName string         `gorm:"not null" json:"display_name"`
	Platform    string         `gorm:"not null" json:"platform"`
	OSVersion   string         `gorm:"not null" json:"os_version"`
	AppVersion  string         `gorm:"not null" json:"app_version"`
	Hardware    datatypes.JSON `gorm:"type:jsonb;not null;default:'{}'" json:"hardware"`
	FirstSeenAt time.Time      `gorm:"not null" json:"first_seen_at"`
	LastSeenAt  time.Time      `gorm:"not null" json:"last_seen_at"`
	RevokedAt   *time.Time     `json:"revoked_at"`
}

type DesktopSession struct {
	ID                uuid.UUID `gorm:"type:uuid;primaryKey"`
	UserID            uuid.UUID `gorm:"type:uuid;index;not null"`
	DeviceID          uuid.UUID `gorm:"type:uuid;index;not null"`
	RefreshTokenHash  string    `gorm:"uniqueIndex;not null"`
	ReporterTokenHash string    `gorm:"uniqueIndex;not null"`
	ReporterExpiresAt time.Time `gorm:"index;not null"`
	ExpiresAt         time.Time `gorm:"index;not null"`
	CreatedAt         time.Time
	LastSeenAt        time.Time
	RotatedAt         *time.Time
	RevokedAt         *time.Time
}

type PasswordResetToken struct {
	ID        uuid.UUID `gorm:"type:uuid;primaryKey"`
	UserID    uuid.UUID `gorm:"type:uuid;index;not null"`
	TokenHash string    `gorm:"uniqueIndex;not null"`
	ExpiresAt time.Time `gorm:"index;not null"`
	CreatedAt time.Time
	UsedAt    *time.Time
}

type TokenCycle struct {
	ID             uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	UserID         uuid.UUID `gorm:"type:uuid;uniqueIndex:token_cycle_user_month;not null" json:"user_id"`
	StartsAt       time.Time `gorm:"uniqueIndex:token_cycle_user_month;not null" json:"starts_at"`
	EndsAt         time.Time `gorm:"not null" json:"ends_at"`
	BaseLimit      int64     `gorm:"not null" json:"base_limit"`
	Adjustment     int64     `gorm:"not null;default:0" json:"adjustment"`
	UsedTokens     int64     `gorm:"not null;default:0" json:"used_tokens"`
	ReservedTokens int64     `gorm:"not null;default:0" json:"reserved_tokens"`
	CreatedAt      time.Time `json:"created_at"`
	UpdatedAt      time.Time `json:"updated_at"`
}

type TokenReservation struct {
	ID        uuid.UUID `gorm:"type:uuid;primaryKey"`
	CycleID   uuid.UUID `gorm:"type:uuid;index;not null"`
	UserID    uuid.UUID `gorm:"type:uuid;index;not null"`
	Provider  string    `gorm:"not null"`
	Model     string    `gorm:"not null"`
	Reserved  int64     `gorm:"not null"`
	CreatedAt time.Time
	SettledAt *time.Time
}

type TokenLedger struct {
	ID            uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	CycleID       uuid.UUID      `gorm:"type:uuid;index;not null" json:"cycle_id"`
	UserID        uuid.UUID      `gorm:"type:uuid;index;not null" json:"user_id"`
	ActorAdminID  *uuid.UUID     `gorm:"type:uuid" json:"actor_admin_id"`
	ReservationID *uuid.UUID     `gorm:"type:uuid" json:"reservation_id"`
	Kind          string         `gorm:"not null" json:"kind"`
	Delta         int64          `gorm:"not null" json:"delta"`
	BalanceAfter  int64          `gorm:"not null" json:"balance_after"`
	Metadata      datatypes.JSON `gorm:"type:jsonb;not null;default:'{}'" json:"metadata"`
	CreatedAt     time.Time      `json:"created_at"`
}

type TelemetrySession struct {
	ID         uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	UserID     uuid.UUID      `gorm:"type:uuid;index;not null" json:"user_id"`
	DeviceID   uuid.UUID      `gorm:"type:uuid;index;not null" json:"device_id"`
	AppVersion string         `gorm:"not null" json:"app_version"`
	BuildID    string         `gorm:"not null" json:"build_id"`
	StartedAt  time.Time      `gorm:"not null" json:"started_at"`
	LastSeenAt time.Time      `gorm:"not null" json:"last_seen_at"`
	EndedAt    *time.Time     `json:"ended_at"`
	EndReason  string         `json:"end_reason"`
	Hardware   datatypes.JSON `gorm:"type:jsonb;not null;default:'{}'" json:"hardware"`
	CreatedAt  time.Time      `json:"created_at"`
}

type TelemetryEvent struct {
	ID         uuid.UUID      `gorm:"type:uuid;primaryKey"`
	EventID    uuid.UUID      `gorm:"type:uuid;uniqueIndex;not null"`
	SessionID  uuid.UUID      `gorm:"type:uuid;index;not null"`
	UserID     uuid.UUID      `gorm:"type:uuid;index;not null"`
	DeviceID   uuid.UUID      `gorm:"type:uuid;index;not null"`
	Kind       string         `gorm:"not null"`
	OccurredAt time.Time      `gorm:"index;not null"`
	Payload    datatypes.JSON `gorm:"type:jsonb;not null;default:'{}'"`
	CreatedAt  time.Time
}

type TelemetrySample struct {
	ID            uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	EventID       uuid.UUID      `gorm:"type:uuid;not null" json:"event_id"`
	SessionID     uuid.UUID      `gorm:"type:uuid;index;not null" json:"session_id"`
	UserID        uuid.UUID      `gorm:"type:uuid;index;not null" json:"user_id"`
	DeviceID      uuid.UUID      `gorm:"type:uuid;index;not null" json:"device_id"`
	RecordedAt    time.Time      `gorm:"primaryKey;not null" json:"recorded_at"`
	ProcessCPU    float64        `json:"process_cpu"`
	SystemCPU     float64        `json:"system_cpu"`
	DSPLoad       float64        `json:"dsp_load"`
	DSPPeak       float64        `json:"dsp_peak"`
	Xruns         int64          `json:"xruns"`
	ResidentBytes int64          `json:"resident_bytes"`
	SampleRate    float64        `json:"sample_rate"`
	BufferFrames  int            `json:"buffer_frames"`
	TrackCount    int            `json:"track_count"`
	ClipCount     int            `json:"clip_count"`
	PluginCount   int            `json:"plugin_count"`
	PlaybackState string         `json:"playback_state"`
	Recording     bool           `json:"recording"`
	Foreground    bool           `json:"foreground"`
	Plugins       datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"plugins"`
}

type CrashReport struct {
	ID            uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	UserID        uuid.UUID      `gorm:"type:uuid;index;not null" json:"user_id"`
	DeviceID      uuid.UUID      `gorm:"type:uuid;index;not null" json:"device_id"`
	SessionID     *uuid.UUID     `gorm:"type:uuid;index" json:"session_id"`
	BuildID       string         `gorm:"not null" json:"build_id"`
	AppVersion    string         `gorm:"not null" json:"app_version"`
	Platform      string         `gorm:"not null" json:"platform"`
	Reason        string         `gorm:"not null" json:"reason"`
	LastPlugin    string         `json:"last_plugin"`
	Metadata      datatypes.JSON `gorm:"type:jsonb;not null;default:'{}'" json:"metadata"`
	ArtifactPath  string         `json:"-"`
	ArtifactBytes int64          `json:"artifact_bytes"`
	SHA256        string         `json:"sha256"`
	OccurredAt    time.Time      `gorm:"not null" json:"occurred_at"`
	CreatedAt     time.Time      `json:"created_at"`
}

type BugReport struct {
	ID           uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	Number       int64     `gorm:"uniqueIndex;autoIncrement" json:"number"`
	UserID       uuid.UUID `gorm:"type:uuid;index;not null" json:"user_id"`
	Title        string    `gorm:"not null" json:"title"`
	Description  string    `gorm:"not null" json:"description"`
	Steps        string    `json:"steps"`
	Expected     string    `json:"expected"`
	Actual       string    `json:"actual"`
	Status       string    `gorm:"not null;default:new" json:"status"`
	InternalNote string    `json:"internal_note,omitempty"`
	CreatedAt    time.Time `json:"created_at"`
	UpdatedAt    time.Time `json:"updated_at"`
}

type BugAttachment struct {
	ID        uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	BugID     uuid.UUID `gorm:"type:uuid;index;not null" json:"bug_id"`
	FileName  string    `gorm:"not null" json:"file_name"`
	MimeType  string    `gorm:"not null" json:"mime_type"`
	Path      string    `gorm:"not null" json:"-"`
	Bytes     int64     `gorm:"not null" json:"bytes"`
	SHA256    string    `gorm:"not null" json:"sha256"`
	CreatedAt time.Time `json:"created_at"`
}

type AdminAuditLog struct {
	ID          uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	AdminUserID uuid.UUID      `gorm:"type:uuid;index;not null" json:"admin_user_id"`
	Action      string         `gorm:"not null" json:"action"`
	TargetType  string         `gorm:"not null" json:"target_type"`
	TargetHash  string         `gorm:"not null" json:"target_hash"`
	Metadata    datatypes.JSON `gorm:"type:jsonb;not null;default:'{}'" json:"metadata"`
	IP          string         `gorm:"not null" json:"ip"`
	CreatedAt   time.Time      `json:"created_at"`
}

const (
	ReleaseDraft     = "draft"
	ReleasePublished = "published"
)

type Release struct {
	ID           uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	Version      *string        `gorm:"uniqueIndex" json:"version"`
	VersionMajor *int           `json:"version_major,omitempty"`
	VersionMinor *int           `json:"version_minor,omitempty"`
	VersionPatch *int           `json:"version_patch,omitempty"`
	Status       string         `gorm:"not null;default:draft" json:"status"`
	SummaryRU    string         `gorm:"not null;default:''" json:"summary_ru"`
	SummaryEN    string         `gorm:"not null;default:''" json:"summary_en"`
	FeaturesRU   datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"features_ru"`
	FeaturesEN   datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"features_en"`
	ChangesRU    datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"changes_ru"`
	ChangesEN    datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"changes_en"`
	FixesRU      datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"fixes_ru"`
	FixesEN      datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"fixes_en"`
	CreatedBy    *uuid.UUID     `gorm:"type:uuid" json:"created_by,omitempty"`
	UpdatedBy    *uuid.UUID     `gorm:"type:uuid" json:"updated_by,omitempty"`
	PublishedAt  *time.Time     `json:"published_at"`
	CreatedAt    time.Time      `json:"created_at"`
	UpdatedAt    time.Time      `json:"updated_at"`
}

type ReleaseArtifact struct {
	ID        uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	ReleaseID uuid.UUID `gorm:"type:uuid;uniqueIndex:release_artifact_kind;not null" json:"release_id"`
	Kind      string    `gorm:"uniqueIndex:release_artifact_kind;not null" json:"kind"`
	FileName  string    `gorm:"not null" json:"file_name"`
	MimeType  string    `gorm:"not null" json:"mime_type"`
	Path      string    `gorm:"not null" json:"-"`
	Bytes     int64     `gorm:"not null" json:"bytes"`
	SHA256    string    `gorm:"not null" json:"sha256"`
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

type ReleaseScreenshot struct {
	ID        uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	ReleaseID uuid.UUID `gorm:"type:uuid;index;not null" json:"release_id"`
	CaptionRU string    `gorm:"not null;default:''" json:"caption_ru"`
	CaptionEN string    `gorm:"not null;default:''" json:"caption_en"`
	SortOrder int       `gorm:"not null;default:0" json:"sort_order"`
	MimeType  string    `gorm:"not null" json:"mime_type"`
	Path      string    `gorm:"not null" json:"-"`
	Bytes     int64     `gorm:"not null" json:"bytes"`
	Width     int       `gorm:"not null" json:"width"`
	Height    int       `gorm:"not null" json:"height"`
	SHA256    string    `gorm:"not null" json:"sha256"`
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

// AIPromptDocument is one of the assistant's instruction documents: the main
// prompt, or one playbook. Edited in the admin panel and served to the desktop,
// which is what lets the assistant's behaviour change without a release.
type AIPromptDocument struct {
	ID        string         `gorm:"primaryKey" json:"id"`
	Kind      string         `gorm:"not null" json:"kind"` // "main" or "playbook"
	Title     string         `gorm:"not null;default:''" json:"title"`
	UseWhen   string         `gorm:"not null;default:''" json:"use_when"`
	Tags      datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"tags"`
	Body      string         `gorm:"not null" json:"body"`
	Enabled   bool           `gorm:"not null;default:true" json:"enabled"`
	UpdatedBy *uuid.UUID     `gorm:"type:uuid" json:"updated_by,omitempty"`
	CreatedAt time.Time      `json:"created_at"`
	UpdatedAt time.Time      `json:"updated_at"`
}

func (AIPromptDocument) TableName() string { return "ai_prompt_documents" }

// AIPromptRevision is the text an edit replaced. Prompts are behaviour, so an
// edit that makes the assistant worse has to be reversible on its own.
type AIPromptRevision struct {
	ID         uuid.UUID      `gorm:"type:uuid;primaryKey" json:"id"`
	DocumentID string         `gorm:"index;not null" json:"document_id"`
	Title      string         `gorm:"not null;default:''" json:"title"`
	UseWhen    string         `gorm:"not null;default:''" json:"use_when"`
	Tags       datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"tags"`
	Body       string         `gorm:"not null" json:"body"`
	UpdatedBy  *uuid.UUID     `gorm:"type:uuid" json:"updated_by,omitempty"`
	CreatedAt  time.Time      `json:"created_at"`
}

func (AIPromptRevision) TableName() string { return "ai_prompt_revisions" }

// AIModel is one administrator-managed model connection. Desktop clients see
// only a public projection while choosing a model. For each provider request,
// an authenticated one-use lease returns the current endpoint and credential
// after the model and token quota have been checked.
type AIModel struct {
	ID               uuid.UUID  `gorm:"type:uuid;primaryKey" json:"id"`
	DisplayName      string     `gorm:"not null" json:"display_name"`
	Provider         string     `gorm:"not null" json:"provider"`
	ModelName        string     `gorm:"not null" json:"model"`
	EndpointURL      string     `gorm:"not null" json:"-"`
	APIKeyCiphertext string     `gorm:"not null" json:"-"`
	Enabled          bool       `gorm:"not null;default:true" json:"enabled"`
	SortOrder        int        `gorm:"not null;default:0" json:"sort_order"`
	UpdatedBy        *uuid.UUID `gorm:"type:uuid" json:"updated_by,omitempty"`
	CreatedAt        time.Time  `json:"created_at"`
	UpdatedAt        time.Time  `json:"updated_at"`
}

func (AIModel) TableName() string { return "ai_models" }
