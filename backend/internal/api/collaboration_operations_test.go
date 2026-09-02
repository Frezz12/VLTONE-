package api

import (
	"testing"
	"time"

	"github.com/google/uuid"
)

func TestOperationSequencerSerializesOneProject(t *testing.T) {
	sequencer := newOperationSequencer()
	projectID := uuid.New()
	firstUnlock := sequencer.lock(projectID)
	entered := make(chan struct{})
	done := make(chan struct{})
	go func() {
		unlock := sequencer.lock(projectID)
		close(entered)
		unlock()
		close(done)
	}()
	select {
	case <-entered:
		t.Fatal("second append entered before the first publish boundary")
	case <-time.After(20 * time.Millisecond):
	}
	firstUnlock()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("second append did not continue after project unlock")
	}
}
