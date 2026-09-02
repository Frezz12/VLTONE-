package collab

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"vltstudio/backend/internal/model"
	"vltstudio/backend/internal/objectstore"
)

type maintenanceFakeStore struct {
	aborts    []string
	deletes   []string
	abortErr  error
	deleteErr error
}

func (f *maintenanceFakeStore) PresignPut(context.Context, string,
	objectstore.ExpectedObject, time.Duration) (objectstore.PresignedRequest, error) {
	return objectstore.PresignedRequest{}, errors.New("unexpected PresignPut")
}

func (f *maintenanceFakeStore) PresignGet(context.Context, string,
	time.Duration) (objectstore.PresignedRequest, error) {
	return objectstore.PresignedRequest{}, errors.New("unexpected PresignGet")
}

func (f *maintenanceFakeStore) CreateMultipart(context.Context, string,
	objectstore.ExpectedObject) (string, error) {
	return "", errors.New("unexpected CreateMultipart")
}

func (f *maintenanceFakeStore) PresignMultipartPart(context.Context, string,
	string, int, int64, time.Duration) (objectstore.PresignedRequest, error) {
	return objectstore.PresignedRequest{}, errors.New("unexpected PresignMultipartPart")
}

func (f *maintenanceFakeStore) ListMultipartParts(context.Context, string,
	string, int) ([]objectstore.UploadedPart, error) {
	return nil, errors.New("unexpected ListMultipartParts")
}

func (f *maintenanceFakeStore) CompleteMultipart(context.Context, string,
	string, []objectstore.UploadedPart) error {
	return errors.New("unexpected CompleteMultipart")
}

func (f *maintenanceFakeStore) AbortMultipart(_ context.Context, key,
	providerUploadID string) error {
	f.aborts = append(f.aborts, key+":"+providerUploadID)
	return f.abortErr
}

func (f *maintenanceFakeStore) VerifyAndPromote(context.Context, string,
	string, objectstore.ExpectedObject) error {
	return errors.New("unexpected VerifyAndPromote")
}

func (f *maintenanceFakeStore) Delete(_ context.Context, key string) error {
	f.deletes = append(f.deletes, key)
	return f.deleteErr
}

func TestCleanupExpiredMultipartAbortsBeforeDeletingStaging(t *testing.T) {
	fake := &maintenanceFakeStore{}
	job := model.ObjectCleanupJob{
		ObjectKey:        "uploads/project/upload",
		ProviderUploadID: "provider-id", AbortMultipart: true, DeleteObject: true,
	}
	if err := cleanupQueuedObject(context.Background(), fake, job); err != nil {
		t.Fatal(err)
	}
	if len(fake.aborts) != 1 || fake.aborts[0] != "uploads/project/upload:provider-id" ||
		len(fake.deletes) != 1 || fake.deletes[0] != job.ObjectKey {
		t.Fatalf("cleanup order/state is incomplete: aborts=%v deletes=%v",
			fake.aborts, fake.deletes)
	}
}

func TestCleanupExpiredMultipartRetriesProviderFailureSafely(t *testing.T) {
	fake := &maintenanceFakeStore{abortErr: objectstore.ErrProvider}
	job := model.ObjectCleanupJob{
		ObjectKey:        "uploads/project/upload",
		ProviderUploadID: "provider-id", AbortMultipart: true, DeleteObject: true,
	}
	if err := cleanupQueuedObject(context.Background(), fake, job); !errors.Is(err, objectstore.ErrProvider) {
		t.Fatalf("provider abort failure was hidden: %v", err)
	}
	if len(fake.deletes) != 0 {
		t.Fatal("staging object was deleted before multipart abort succeeded")
	}
	fake.abortErr = objectstore.ErrMultipartNotFound
	if err := cleanupQueuedObject(context.Background(), fake, job); err != nil {
		t.Fatalf("idempotent missing multipart was not accepted: %v", err)
	}
	if len(fake.deletes) != 1 {
		t.Fatal("retry did not remove staging object")
	}
}

func TestObjectCleanupRetryIsBoundedAndErrorIsSafeForStorage(t *testing.T) {
	if objectCleanupRetryDelay(1) != 5*time.Second ||
		objectCleanupRetryDelay(100) != time.Hour {
		t.Fatal("cleanup retry backoff is not bounded")
	}
	errorText := cleanupErrorText(errors.New("provider\nfailed\x00" + string(make([]byte, 600))))
	if len([]rune(errorText)) > 512 || strings.ContainsAny(errorText, "\x00\r\n") {
		t.Fatalf("unsafe cleanup error text %q", errorText)
	}
}

func TestStorageMaintenanceBounds(t *testing.T) {
	for _, limit := range []int{0, MaximumStorageBatch + 1} {
		if err := validateStorageBatch(limit); !errors.Is(err, ErrValidation) {
			t.Fatalf("invalid batch %d returned %v", limit, err)
		}
	}
	if err := validateStorageBatch(128); err != nil {
		t.Fatal(err)
	}
}
