package model

import (
	"github.com/google/uuid"
	"time"
)

type ForumFollow struct {
	UserID    uuid.UUID `json:"user_id" gorm:"primaryKey"`
	AuthorID  uuid.UUID `json:"author_id" gorm:"primaryKey"`
	CreatedAt time.Time `json:"created_at"`
}
type ForumBookmark struct {
	UserID     uuid.UUID `json:"-" gorm:"primaryKey"`
	TargetType string    `json:"target_type" gorm:"primaryKey"`
	TargetID   uuid.UUID `json:"target_id" gorm:"primaryKey"`
	CreatedAt  time.Time `json:"created_at"`
}
type ForumNotification struct {
	ID         uuid.UUID  `json:"id" gorm:"primaryKey"`
	UserID     uuid.UUID  `json:"-"`
	ActorID    uuid.UUID  `json:"actor_id"`
	Kind       string     `json:"kind"`
	TargetType string     `json:"target_type"`
	TargetID   uuid.UUID  `json:"target_id"`
	ReadAt     *time.Time `json:"read_at"`
	CreatedAt  time.Time  `json:"created_at"`
}
type ForumReport struct {
	ID         uuid.UUID `json:"id" gorm:"primaryKey"`
	UserID     uuid.UUID `json:"user_id"`
	TargetType string    `json:"target_type"`
	TargetID   uuid.UUID `json:"target_id"`
	Reason     string    `json:"reason"`
	Status     string    `json:"status"`
	CreatedAt  time.Time `json:"created_at"`
}
type ForumAttachment struct {
	ID               uuid.UUID  `json:"id" gorm:"primaryKey"`
	OwnerID          uuid.UUID  `json:"-"`
	Name             string     `json:"name"`
	Bytes            int64      `json:"bytes"`
	SHA256           string     `json:"sha256"`
	ContentType      string     `json:"content_type"`
	Kind             string     `json:"kind"`
	ResourceType     string     `json:"resource_type"`
	MinimumVersion   string     `json:"minimum_version"`
	Status           string     `json:"status"`
	MediaStatus      string     `json:"media_status"`
	MediaKey         string     `json:"-"`
	MediaError       string     `json:"media_error"`
	ObjectKey        string     `json:"-"`
	ProviderUploadID string     `json:"-"`
	PartBytes        int64      `json:"-"`
	UploadTokenHash  string     `json:"-"`
	ExpiresAt        time.Time  `json:"-"`
	ArticleID        *uuid.UUID `json:"article_id,omitempty"`
	TopicID          *uuid.UUID `json:"topic_id,omitempty"`
	PostID           *uuid.UUID `json:"post_id,omitempty"`
	CommentID        *uuid.UUID `json:"comment_id,omitempty"`
	CreatedAt        time.Time  `json:"created_at"`
	UpdatedAt        time.Time  `json:"updated_at"`
}
type ForumMediaJob struct {
	AttachmentID uuid.UUID `gorm:"primaryKey"`
	Attempts     int
	AvailableAt  time.Time
	LeaseUntil   *time.Time
	LeaseID      *uuid.UUID
	LastError    string
	CreatedAt    time.Time
}
type ForumSSOCode struct {
	ID              uuid.UUID `gorm:"primaryKey"`
	TokenHash       string
	UserID          uuid.UUID
	DeviceID        *uuid.UUID
	SourceSessionID *uuid.UUID
	Challenge       string
	State           string
	RedirectURI     string
	ExpiresAt       time.Time
	ConsumedAt      *time.Time
}

func (ForumSSOCode) TableName() string { return "forum_sso_codes" }
