package model

import (
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
)

const (
	ForumStatusPublished = "published"
	ForumStatusHidden    = "hidden"
	ForumStatusDraft     = "draft"

	ForumTargetTopic   = "topic"
	ForumTargetPost    = "post"
	ForumTargetArticle = "article"
	ForumTargetComment = "comment"
)

type ForumSection struct {
	ID            uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	Slug          string    `gorm:"uniqueIndex;not null" json:"slug"`
	TitleRU       string    `gorm:"not null;default:''" json:"title_ru"`
	TitleEN       string    `gorm:"not null;default:''" json:"title_en"`
	DescriptionRU string    `gorm:"not null;default:''" json:"description_ru"`
	DescriptionEN string    `gorm:"not null;default:''" json:"description_en"`
	SortOrder     int       `gorm:"not null;default:0" json:"sort_order"`
	CreatedAt     time.Time `json:"created_at"`
	UpdatedAt     time.Time `json:"updated_at"`
}

// ForumProfile is the personal page backing the forum. It is created lazily on
// the first visit so an existing account always has a page to land on.
type ForumProfile struct {
	UserID          uuid.UUID      `gorm:"type:uuid;primaryKey" json:"user_id"`
	About           string         `gorm:"not null;default:''" json:"about"`
	Location        string         `gorm:"not null;default:''" json:"location"`
	FamilyStatus    string         `gorm:"not null;default:''" json:"family_status"`
	Interests       string         `gorm:"not null;default:''" json:"interests"`
	Links           datatypes.JSON `gorm:"type:jsonb;not null;default:'[]'" json:"links"`
	Signature       string         `gorm:"not null;default:''" json:"signature"`
	ProfileComplete bool           `gorm:"not null;default:false" json:"profile_completed"`
	CreatedAt       time.Time      `json:"created_at"`
	UpdatedAt       time.Time      `json:"updated_at"`
}

type ForumTopic struct {
	ID         uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	SectionID  uuid.UUID `gorm:"type:uuid;index;not null" json:"section_id"`
	AuthorID   uuid.UUID `gorm:"type:uuid;index;not null" json:"author_id"`
	Title      string    `gorm:"not null" json:"title"`
	Body       string    `gorm:"not null" json:"body"`
	Pinned     bool      `gorm:"not null;default:false" json:"pinned"`
	Locked     bool      `gorm:"not null;default:false" json:"locked"`
	Status     string    `gorm:"not null;default:published" json:"status"`
	ReplyCount int       `gorm:"not null;default:0" json:"reply_count"`
	LikeCount  int       `gorm:"not null;default:0" json:"like_count"`
	ViewCount  int64     `gorm:"not null;default:0" json:"view_count"`
	LastPostAt time.Time `gorm:"not null;default:now()" json:"last_post_at"`
	CreatedAt  time.Time `json:"created_at"`
	UpdatedAt  time.Time `json:"updated_at"`
}

type ForumPost struct {
	ID        uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	TopicID   uuid.UUID `gorm:"type:uuid;index;not null" json:"topic_id"`
	AuthorID  uuid.UUID `gorm:"type:uuid;index;not null" json:"author_id"`
	Body      string    `gorm:"not null" json:"body"`
	Status    string    `gorm:"not null;default:published" json:"status"`
	LikeCount int       `gorm:"not null;default:0" json:"like_count"`
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

type ForumArticle struct {
	ID          uuid.UUID  `gorm:"type:uuid;primaryKey" json:"id"`
	AuthorID    uuid.UUID  `gorm:"type:uuid;index;not null" json:"author_id"`
	Title       string     `gorm:"not null" json:"title"`
	Excerpt     string     `gorm:"not null;default:''" json:"excerpt"`
	Body        string     `gorm:"not null" json:"body"`
	Status      string     `gorm:"not null;default:published" json:"status"`
	LikeCount   int        `gorm:"not null;default:0" json:"like_count"`
	CommentCount int       `gorm:"not null;default:0" json:"comment_count"`
	PublishedAt *time.Time `json:"published_at"`
	CreatedAt   time.Time  `json:"created_at"`
	UpdatedAt   time.Time  `json:"updated_at"`
}

type ForumComment struct {
	ID        uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	ArticleID uuid.UUID `gorm:"type:uuid;index;not null" json:"article_id"`
	AuthorID  uuid.UUID `gorm:"type:uuid;index;not null" json:"author_id"`
	Body      string    `gorm:"not null" json:"body"`
	Status    string    `gorm:"not null;default:published" json:"status"`
	LikeCount int       `gorm:"not null;default:0" json:"like_count"`
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

type ForumReaction struct {
	ID         uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	UserID     uuid.UUID `gorm:"type:uuid;not null" json:"user_id"`
	TargetType string    `gorm:"not null" json:"target_type"`
	TargetID   uuid.UUID `gorm:"type:uuid;not null" json:"target_id"`
	CreatedAt  time.Time `json:"created_at"`
}
