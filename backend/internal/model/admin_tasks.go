package model

import (
	"github.com/google/uuid"
	"time"
)

type AdminTask struct {
	ID          uuid.UUID  `gorm:"type:uuid;primaryKey" json:"id"`
	Number      int64      `gorm:"autoIncrement" json:"number"`
	Title       string     `json:"title"`
	Description string     `json:"description"`
	Status      string     `json:"status"`
	Priority    string     `json:"priority"`
	AuthorID    uuid.UUID  `json:"author_id"`
	AssigneeID  *uuid.UUID `json:"assignee_id"`
	DueDate     *time.Time `gorm:"type:date" json:"due_date"`
	Version     int        `json:"version"`
	CreatedAt   time.Time  `json:"created_at"`
	UpdatedAt   time.Time  `json:"updated_at"`
}
type AdminTaskComment struct {
	ID        uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	TaskID    uuid.UUID `json:"task_id"`
	AuthorID  uuid.UUID `json:"author_id"`
	Body      string    `json:"body"`
	CreatedAt time.Time `json:"created_at"`
}
type AdminTaskAttachment struct {
	ID        uuid.UUID `gorm:"type:uuid;primaryKey" json:"id"`
	TaskID    uuid.UUID `json:"task_id"`
	AuthorID  uuid.UUID `json:"author_id"`
	Name      string    `json:"name"`
	Size      int64     `json:"size"`
	Data      []byte    `json:"-"`
	CreatedAt time.Time `json:"created_at"`
}
