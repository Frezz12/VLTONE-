package collab

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/google/uuid"
)

func nextRoomMessage(t *testing.T, subscription *RoomSubscription) RoomMessage {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	message, err := subscription.Next(ctx)
	if err != nil {
		t.Fatalf("next room message: %v", err)
	}
	return message
}

func TestRoomBusDurableOrderAndSenderExclusion(t *testing.T) {
	bus := NewInProcessRoomBus()
	projectID, senderID := uuid.New(), uuid.New()
	sender, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(), senderID, 4, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	receiver, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(), uuid.New(), 4, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	bus.Publish(projectID, senderID, RoomMessage{Data: []byte("one")})
	bus.Publish(projectID, senderID, RoomMessage{Data: []byte("two")})
	if got := string(nextRoomMessage(t, receiver).Data); got != "one" {
		t.Fatalf("first message = %q", got)
	}
	if got := string(nextRoomMessage(t, receiver).Data); got != "two" {
		t.Fatalf("second message = %q", got)
	}
	select {
	case <-sender.Done():
		t.Fatal("sender was unexpectedly disconnected")
	default:
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Millisecond)
	defer cancel()
	if _, err := sender.Next(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("sender received own message: %v", err)
	}
}

func TestRoomBusEphemeralIsLatestOnlyPerKey(t *testing.T) {
	bus := NewInProcessRoomBus()
	projectID := uuid.New()
	receiver, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(), uuid.New(), 4, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	bus.Publish(projectID, uuid.Nil, RoomMessage{
		Data: []byte("old cursor"), Ephemeral: true, EphemeralKey: "cursor:a",
	})
	bus.Publish(projectID, uuid.Nil, RoomMessage{
		Data: []byte("new cursor"), Ephemeral: true, EphemeralKey: "cursor:a",
	})
	if got := string(nextRoomMessage(t, receiver).Data); got != "new cursor" {
		t.Fatalf("ephemeral message = %q", got)
	}
}

func TestRoomBusDisconnectsSlowDurableConsumer(t *testing.T) {
	bus := NewInProcessRoomBus()
	projectID := uuid.New()
	receiver, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(), uuid.New(), 1, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	bus.Publish(projectID, uuid.Nil, RoomMessage{Data: []byte("one")})
	bus.Publish(projectID, uuid.Nil, RoomMessage{Data: []byte("two")})
	select {
	case <-receiver.Done():
	case <-time.After(time.Second):
		t.Fatal("slow consumer was not disconnected")
	}
	if receiver.CloseInfo().Code != "slow_consumer" {
		t.Fatalf("close code = %q", receiver.CloseInfo().Code)
	}
}

func TestRoomBusForcedDeviceDisconnect(t *testing.T) {
	bus := NewInProcessRoomBus()
	deviceID := uuid.New()
	receiver, err := bus.Subscribe(uuid.New(), uuid.New(), deviceID, uuid.New(), uuid.New(), 2, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	bus.DisconnectDevice(deviceID, RoomClose{Code: "device_revoked"})
	select {
	case <-receiver.Done():
	case <-time.After(time.Second):
		t.Fatal("device subscription was not disconnected")
	}
	if receiver.CloseInfo().Code != "device_revoked" {
		t.Fatalf("close code = %q", receiver.CloseInfo().Code)
	}
}

func TestRoomBusReplacesDuplicateParticipantConnection(t *testing.T) {
	bus := NewInProcessRoomBus()
	projectID, userID, deviceID, participantID := uuid.New(), uuid.New(), uuid.New(), uuid.New()
	first, err := bus.Subscribe(projectID, userID, deviceID, uuid.New(), participantID, 2, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	second, err := bus.Subscribe(projectID, userID, deviceID, uuid.New(), participantID, 2, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	select {
	case <-first.Done():
	case <-time.After(time.Second):
		t.Fatal("older participant connection was not replaced")
	}
	if first.CloseInfo().Code != "connection_replaced" {
		t.Fatalf("replacement close code = %q", first.CloseInfo().Code)
	}
	bus.Publish(projectID, uuid.Nil, RoomMessage{Data: []byte("current")})
	if got := string(nextRoomMessage(t, second).Data); got != "current" {
		t.Fatalf("replacement message = %q", got)
	}
}

func TestRoomBusDisconnectsOnlyMatchingDesktopSession(t *testing.T) {
	bus := NewInProcessRoomBus()
	projectID, userID, deviceID := uuid.New(), uuid.New(), uuid.New()
	oldSessionID, currentSessionID := uuid.New(), uuid.New()
	oldSubscription, err := bus.Subscribe(projectID, userID, deviceID,
		oldSessionID, uuid.New(), 2, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	currentSubscription, err := bus.Subscribe(projectID, userID, deviceID,
		currentSessionID, uuid.New(), 2, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	bus.DisconnectDesktopSession(oldSessionID, RoomClose{Code: "session_rotated"})
	select {
	case <-oldSubscription.Done():
	case <-time.After(time.Second):
		t.Fatal("rotated desktop session was not disconnected")
	}
	select {
	case <-currentSubscription.Done():
		t.Fatal("current desktop session was disconnected with the rotated session")
	default:
	}
}

func TestRoomBusClosePreemptsBufferedDurableMessages(t *testing.T) {
	bus := NewInProcessRoomBus()
	subscription, err := bus.Subscribe(uuid.New(), uuid.New(), uuid.New(),
		uuid.New(), uuid.New(), 2, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	if !subscription.Deliver(RoomMessage{Data: []byte("must not leak")}) {
		t.Fatal("failed to enqueue durable message")
	}
	subscription.Close(RoomClose{Code: "session_rotated"})
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if _, err := subscription.Next(ctx); !errors.Is(err, ErrRoomSubscriptionClosed) {
		t.Fatalf("closed subscription drained buffered data: %v", err)
	}
}

func TestRoomBusDisconnectsWhenQueuedDurableBytesExceedBudget(t *testing.T) {
	bus := NewInProcessRoomBus()
	projectID := uuid.New()
	receiver, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(),
		uuid.New(), 8, 5)
	if err != nil {
		t.Fatal(err)
	}
	bus.Publish(projectID, uuid.Nil, RoomMessage{Data: []byte("123")})
	bus.Publish(projectID, uuid.Nil, RoomMessage{Data: []byte("456")})
	select {
	case <-receiver.Done():
	case <-time.After(time.Second):
		t.Fatal("byte-budget slow consumer was not disconnected")
	}
	if receiver.CloseInfo().Code != "slow_consumer" {
		t.Fatalf("close code = %q", receiver.CloseInfo().Code)
	}
}

func TestRoomBusEphemeralReplacementSharesByteBudget(t *testing.T) {
	bus := NewInProcessRoomBus()
	projectID := uuid.New()
	receiver, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(),
		uuid.New(), 8, 8)
	if err != nil {
		t.Fatal(err)
	}
	bus.Publish(projectID, uuid.Nil, RoomMessage{
		Data: []byte("123456"), Ephemeral: true, EphemeralKey: "cursor:a",
	})
	bus.Publish(projectID, uuid.Nil, RoomMessage{
		Data: []byte("12"), Ephemeral: true, EphemeralKey: "cursor:a",
	})
	select {
	case <-receiver.Done():
		t.Fatal("latest-only replacement was charged twice")
	default:
	}
	bus.Publish(projectID, uuid.Nil, RoomMessage{
		Data: []byte("1234567"), Ephemeral: true, EphemeralKey: "cursor:b",
	})
	select {
	case <-receiver.Done():
	case <-time.After(time.Second):
		t.Fatal("ephemeral byte-budget slow consumer was not disconnected")
	}
}

func TestRoomBusDeliversServerControlToExactParticipant(t *testing.T) {
	bus := NewInProcessRoomBus()
	projectID, targetID := uuid.New(), uuid.New()
	target, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(),
		targetID, 4, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	other, err := bus.Subscribe(projectID, uuid.New(), uuid.New(), uuid.New(),
		uuid.New(), 4, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	if !bus.DeliverParticipant(projectID, targetID,
		RoomMessage{Data: []byte("snapshot.requested")}) {
		t.Fatal("targeted delivery was not accepted")
	}
	if got := string(nextRoomMessage(t, target).Data); got != "snapshot.requested" {
		t.Fatalf("targeted message = %q", got)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Millisecond)
	defer cancel()
	if _, err := other.Next(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("non-target participant received control message: %v", err)
	}
	if bus.DeliverParticipant(projectID, uuid.New(), RoomMessage{Data: []byte("x")}) {
		t.Fatal("delivery to an absent participant succeeded")
	}
}

func TestRoomBusFinalMessageDrainsBeforeGracefulClose(t *testing.T) {
	bus := NewInProcessRoomBus()
	projectID := uuid.New()
	subscription, err := bus.Subscribe(projectID, uuid.New(), uuid.New(),
		uuid.New(), uuid.New(), 4, DefaultRoomQueueBytes)
	if err != nil {
		t.Fatal(err)
	}
	bus.Publish(projectID, uuid.Nil, RoomMessage{Data: []byte("committed")})
	bus.PublishFinal(projectID, RoomMessage{Data: []byte("session.ended")},
		RoomClose{Code: "session_ended"})
	select {
	case <-subscription.Done():
		t.Fatal("room closed before terminal message was drained")
	default:
	}
	if got := string(nextRoomMessage(t, subscription).Data); got != "committed" {
		t.Fatalf("first drained message = %q", got)
	}
	if got := string(nextRoomMessage(t, subscription).Data); got != "session.ended" {
		t.Fatalf("terminal message = %q", got)
	}
	select {
	case <-subscription.Done():
	case <-time.After(time.Second):
		t.Fatal("room did not close after terminal message")
	}
	if subscription.CloseInfo().Code != "session_ended" {
		t.Fatalf("terminal close code = %q", subscription.CloseInfo().Code)
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if _, err := subscription.Next(ctx); !errors.Is(err, ErrRoomSubscriptionClosed) {
		t.Fatalf("closed room returned another message: %v", err)
	}
}
