package collab

import (
	"context"
	"errors"
	"sort"
	"sync"
	"sync/atomic"

	"github.com/google/uuid"
)

const (
	DefaultRoomDurableQueue = 128
	DefaultRoomQueueBytes   = int64(8 << 20)
	MaximumRoomQueueBytes   = int64(64 << 20)
	maxEphemeralKinds       = 32
)

var ErrRoomSubscriptionClosed = errors.New("room subscription closed")

// RoomMessage is already encoded protocol data. The bus intentionally does
// not understand the musical document or WebSocket framing, which keeps the
// single-process implementation replaceable by Redis/NATS later.
type RoomMessage struct {
	Data      []byte
	Ephemeral bool
	// EphemeralKey coalesces high-rate state such as one participant's cursor.
	// It is ignored for durable messages.
	EphemeralKey string
}

type RoomClose struct {
	Code   string
	Reason string
}

// RoomBusStats contains only aggregate transport state. It is safe to expose
// through process metrics because it contains no project or participant IDs.
type RoomBusStats struct {
	Rooms       int
	Sockets     int
	QueuedBytes int64
}

type RoomBus interface {
	Subscribe(projectID, userID, deviceID, desktopSessionID, participantID uuid.UUID,
		durableCapacity int, queueByteCapacity int64) (*RoomSubscription, error)
	Publish(projectID, senderParticipantID uuid.UUID, message RoomMessage)
	// DeliverParticipant sends server-control traffic to one exact live
	// participant. Snapshot requests contain no document data, but must not ask
	// every editor to upload the host's canonical state.
	DeliverParticipant(projectID, participantID uuid.UUID, message RoomMessage) bool
	ConnectedParticipants(projectID uuid.UUID) []uuid.UUID
	Stats() RoomBusStats
	// PublishFinal atomically detaches a room and drains one final durable
	// protocol message before each socket observes the close. Revocations and
	// server shutdown continue to use immediate ShutdownProject/Shutdown.
	PublishFinal(projectID uuid.UUID, message RoomMessage, close RoomClose)
	DisconnectParticipant(participantID uuid.UUID, close RoomClose)
	DisconnectProjectUser(projectID, userID uuid.UUID, close RoomClose)
	DisconnectDevice(deviceID uuid.UUID, close RoomClose)
	DisconnectDesktopSession(desktopSessionID uuid.UUID, close RoomClose)
	DisconnectUser(userID uuid.UUID, close RoomClose)
	ShutdownProject(projectID uuid.UUID, close RoomClose)
	Shutdown(close RoomClose)
}

func (b *InProcessRoomBus) Stats() RoomBusStats {
	b.mu.RLock()
	rooms := len(b.subscriptions)
	b.mu.RUnlock()
	return RoomBusStats{Rooms: rooms, Sockets: int(b.socketCount.Load()),
		QueuedBytes: b.queuedBytes.Load()}
}

func (b *InProcessRoomBus) ConnectedParticipants(projectID uuid.UUID) []uuid.UUID {
	b.mu.RLock()
	defer b.mu.RUnlock()
	room := b.subscriptions[projectID]
	participants := make([]uuid.UUID, 0, len(room))
	for _, subscription := range room {
		participants = append(participants, subscription.participantID)
	}
	sort.Slice(participants, func(i, j int) bool {
		return participants[i].String() < participants[j].String()
	})
	return participants
}

// InProcessRoomBus is the v1 single-instance room fanout. Durable messages use
// bounded FIFO queues; a slow consumer is disconnected instead of being
// allowed to exhaust server memory. Ephemeral values are latest-only per key.
type InProcessRoomBus struct {
	mu            sync.RWMutex
	subscriptions map[uuid.UUID]map[uuid.UUID]*RoomSubscription
	socketCount   atomic.Int64
	queuedBytes   atomic.Int64
	closed        bool
}

func NewInProcessRoomBus() *InProcessRoomBus {
	return &InProcessRoomBus{
		subscriptions: make(map[uuid.UUID]map[uuid.UUID]*RoomSubscription),
	}
}

type RoomSubscription struct {
	bus              *InProcessRoomBus
	id               uuid.UUID
	projectID        uuid.UUID
	userID           uuid.UUID
	deviceID         uuid.UUID
	desktopSessionID uuid.UUID
	participantID    uuid.UUID
	durable          chan RoomMessage
	wakeEphemeral    chan struct{}
	done             chan struct{}

	mu                sync.Mutex
	ephemeral         map[string]RoomMessage
	queueByteCapacity int64
	queuedBytes       atomic.Int64
	closeInfo         RoomClose
	closing           bool
	closed            bool
}

func (b *InProcessRoomBus) Subscribe(projectID, userID, deviceID,
	desktopSessionID, participantID uuid.UUID,
	durableCapacity int, queueByteCapacity int64) (*RoomSubscription, error) {
	if projectID == uuid.Nil || userID == uuid.Nil || deviceID == uuid.Nil ||
		desktopSessionID == uuid.Nil || participantID == uuid.Nil {
		return nil, invalidf("room subscription identity is incomplete")
	}
	if durableCapacity <= 0 {
		durableCapacity = DefaultRoomDurableQueue
	}
	if durableCapacity > 4096 {
		return nil, invalidf("room durable queue is too large")
	}
	if queueByteCapacity <= 0 {
		queueByteCapacity = DefaultRoomQueueBytes
	}
	if queueByteCapacity > MaximumRoomQueueBytes {
		return nil, invalidf("room queue byte capacity is too large")
	}
	subscription := &RoomSubscription{
		bus: b, id: uuid.New(), projectID: projectID, userID: userID,
		deviceID: deviceID, desktopSessionID: desktopSessionID,
		participantID: participantID,
		durable:       make(chan RoomMessage, durableCapacity),
		wakeEphemeral: make(chan struct{}, 1), done: make(chan struct{}),
		ephemeral:         make(map[string]RoomMessage),
		queueByteCapacity: queueByteCapacity,
	}
	b.mu.Lock()
	if b.closed {
		b.mu.Unlock()
		return nil, ErrRoomSubscriptionClosed
	}
	room := b.subscriptions[projectID]
	if room == nil {
		room = make(map[uuid.UUID]*RoomSubscription)
		b.subscriptions[projectID] = room
	}
	// A project participant is a user/device membership, not a socket. Reusing
	// that membership for unbounded parallel WebSockets would bypass the room
	// participant limit and allocate one durable queue per connection. A fresh
	// connection atomically replaces the previous transport while retaining the
	// same durable session membership.
	var replaced []*RoomSubscription
	for id, existing := range room {
		if existing.participantID == participantID {
			delete(room, id)
			replaced = append(replaced, existing)
		}
	}
	room[subscription.id] = subscription
	b.socketCount.Add(1)
	b.mu.Unlock()
	for _, existing := range replaced {
		existing.markClosed(RoomClose{
			Code: "connection_replaced", Reason: "newer participant connection opened",
		})
	}
	return subscription, nil
}

func (b *InProcessRoomBus) Publish(projectID, senderParticipantID uuid.UUID,
	message RoomMessage) {
	if projectID == uuid.Nil || len(message.Data) == 0 {
		return
	}
	b.mu.RLock()
	room := b.subscriptions[projectID]
	targets := make([]*RoomSubscription, 0, len(room))
	for _, subscription := range room {
		if senderParticipantID != uuid.Nil &&
			subscription.participantID == senderParticipantID {
			continue
		}
		targets = append(targets, subscription)
	}
	b.mu.RUnlock()

	for _, subscription := range targets {
		copyMessage := message
		copyMessage.Data = append([]byte(nil), message.Data...)
		if message.Ephemeral {
			if !subscription.enqueueEphemeral(copyMessage) {
				subscription.Close(RoomClose{
					Code: "slow_consumer", Reason: "room queue byte budget exceeded",
				})
			}
			continue
		}
		if !subscription.enqueueDurable(copyMessage) {
			subscription.Close(RoomClose{
				Code: "slow_consumer", Reason: "durable room queue exceeded",
			})
		}
	}
}

func (b *InProcessRoomBus) DeliverParticipant(projectID, participantID uuid.UUID,
	message RoomMessage) bool {
	if projectID == uuid.Nil || participantID == uuid.Nil || len(message.Data) == 0 {
		return false
	}
	b.mu.RLock()
	room := b.subscriptions[projectID]
	var target *RoomSubscription
	for _, subscription := range room {
		if subscription.participantID == participantID {
			target = subscription
			break
		}
	}
	b.mu.RUnlock()
	if target == nil {
		return false
	}
	return target.Deliver(message)
}

func (b *InProcessRoomBus) PublishFinal(projectID uuid.UUID, message RoomMessage,
	closeInfo RoomClose) {
	if projectID == uuid.Nil || len(message.Data) == 0 {
		return
	}
	b.mu.Lock()
	room := b.subscriptions[projectID]
	targets := make([]*RoomSubscription, 0, len(room))
	for _, subscription := range room {
		targets = append(targets, subscription)
	}
	delete(b.subscriptions, projectID)
	b.mu.Unlock()
	for _, subscription := range targets {
		copyMessage := message
		copyMessage.Data = append([]byte(nil), message.Data...)
		if !subscription.enqueueFinal(copyMessage, closeInfo) {
			subscription.markClosed(RoomClose{
				Code: "slow_consumer", Reason: "final room message exceeded queue",
			})
		}
	}
}

func (s *RoomSubscription) Next(ctx context.Context) (RoomMessage, error) {
	for {
		// Revocation/replacement must win over already-buffered room data. Without
		// this check a closed socket could drain up to the full durable queue while
		// its read loop remained authorized to submit more operations.
		select {
		case <-s.done:
			return RoomMessage{}, ErrRoomSubscriptionClosed
		default:
		}

		// Preserve durable FIFO ordering and give committed operations priority
		// over cursor/transport updates whenever both are ready.
		select {
		case message := <-s.durable:
			return s.finishDurableDelivery(message)
		default:
		}
		select {
		case message := <-s.durable:
			return s.finishDurableDelivery(message)
		case <-s.wakeEphemeral:
			if message, ok := s.takeEphemeral(); ok {
				select {
				case <-s.done:
					return RoomMessage{}, ErrRoomSubscriptionClosed
				default:
					return message, nil
				}
			}
		case <-s.done:
			return RoomMessage{}, ErrRoomSubscriptionClosed
		case <-ctx.Done():
			return RoomMessage{}, ctx.Err()
		}
	}
}

func (s *RoomSubscription) Done() <-chan struct{} { return s.done }

func (s *RoomSubscription) ParticipantID() uuid.UUID { return s.participantID }

// Deliver queues a server response for this connection only. It follows the
// same bounded/slow-consumer rules as room fanout.
func (s *RoomSubscription) Deliver(message RoomMessage) bool {
	message.Data = append([]byte(nil), message.Data...)
	if message.Ephemeral {
		if s.enqueueEphemeral(message) {
			return true
		}
		s.Close(RoomClose{
			Code: "slow_consumer", Reason: "room queue byte budget exceeded",
		})
		return false
	}
	if s.enqueueDurable(message) {
		return true
	}
	s.Close(RoomClose{
		Code: "slow_consumer", Reason: "durable room queue exceeded",
	})
	return false
}

func (s *RoomSubscription) CloseInfo() RoomClose {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.closeInfo
}

func (s *RoomSubscription) Close(closeInfo RoomClose) {
	if s.bus != nil {
		s.bus.remove(s, closeInfo)
	}
}

func (s *RoomSubscription) enqueueDurable(message RoomMessage) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed || s.closing {
		return true
	}
	messageBytes := int64(len(message.Data))
	if !s.adjustQueuedBytes(messageBytes) {
		return false
	}
	select {
	case s.durable <- message:
		return true
	default:
		s.adjustQueuedBytes(-messageBytes)
		return false
	}
}

func (s *RoomSubscription) enqueueFinal(message RoomMessage,
	closeInfo RoomClose) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed || s.closing {
		return true
	}
	messageBytes := int64(len(message.Data))
	if !s.adjustQueuedBytes(messageBytes) {
		return false
	}
	select {
	case s.durable <- message:
		s.closing = true
		s.closeInfo = closeInfo
		for key, ephemeral := range s.ephemeral {
			s.adjustQueuedBytes(-int64(len(ephemeral.Data)))
			delete(s.ephemeral, key)
		}
		return true
	default:
		s.adjustQueuedBytes(-messageBytes)
		return false
	}
}

func (s *RoomSubscription) enqueueEphemeral(message RoomMessage) bool {
	key := message.EphemeralKey
	if key == "" {
		key = "ephemeral"
	}
	s.mu.Lock()
	if s.closed || s.closing {
		s.mu.Unlock()
		return true
	}
	existing, exists := s.ephemeral[key]
	if !exists && len(s.ephemeral) >= maxEphemeralKinds {
		s.mu.Unlock()
		return true
	}
	delta := int64(len(message.Data) - len(existing.Data))
	if !s.adjustQueuedBytes(delta) {
		s.mu.Unlock()
		return false
	}
	s.ephemeral[key] = message
	s.mu.Unlock()
	select {
	case s.wakeEphemeral <- struct{}{}:
	default:
	}
	return true
}

func (s *RoomSubscription) finishDurableDelivery(message RoomMessage) (
	RoomMessage, error) {
	s.releaseQueuedBytes(len(message.Data))
	select {
	case <-s.done:
		return RoomMessage{}, ErrRoomSubscriptionClosed
	default:
	}
	s.mu.Lock()
	closeAfterDelivery := s.closing && len(s.durable) == 0
	closeInfo := s.closeInfo
	s.mu.Unlock()
	if closeAfterDelivery {
		// The caller already owns the returned bytes. Closing in this defer makes
		// the next Next call fail without discarding the terminal message.
		defer s.markClosed(closeInfo)
	}
	return message, nil
}

func (s *RoomSubscription) takeEphemeral() (RoomMessage, bool) {
	s.mu.Lock()
	if len(s.ephemeral) == 0 {
		s.mu.Unlock()
		return RoomMessage{}, false
	}
	keys := make([]string, 0, len(s.ephemeral))
	for key := range s.ephemeral {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	message := s.ephemeral[keys[0]]
	delete(s.ephemeral, keys[0])
	s.adjustQueuedBytes(-int64(len(message.Data)))
	more := len(s.ephemeral) != 0
	s.mu.Unlock()
	if more {
		select {
		case s.wakeEphemeral <- struct{}{}:
		default:
		}
	}
	return message, true
}

func (s *RoomSubscription) releaseQueuedBytes(messageBytes int) {
	s.adjustQueuedBytes(-int64(messageBytes))
}

func (s *RoomSubscription) adjustQueuedBytes(delta int64) bool {
	for {
		current := s.queuedBytes.Load()
		next := current + delta
		if next > s.queueByteCapacity {
			return false
		}
		if next < 0 {
			next = 0
		}
		if s.queuedBytes.CompareAndSwap(current, next) {
			if s.bus != nil {
				s.bus.queuedBytes.Add(next - current)
			}
			return true
		}
	}
}

func (b *InProcessRoomBus) remove(subscription *RoomSubscription,
	closeInfo RoomClose) {
	b.mu.Lock()
	if room := b.subscriptions[subscription.projectID]; room != nil {
		delete(room, subscription.id)
		if len(room) == 0 {
			delete(b.subscriptions, subscription.projectID)
		}
	}
	b.mu.Unlock()
	subscription.markClosed(closeInfo)
}

func (s *RoomSubscription) markClosed(closeInfo RoomClose) {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return
	}
	s.closed = true
	s.closeInfo = closeInfo
	queuedBytes := s.queuedBytes.Swap(0)
	if s.bus != nil {
		s.bus.queuedBytes.Add(-queuedBytes)
		s.bus.socketCount.Add(-1)
	}
	close(s.done)
	s.mu.Unlock()
}

func (b *InProcessRoomBus) disconnectMatching(match func(*RoomSubscription) bool,
	closeInfo RoomClose) {
	b.mu.RLock()
	var targets []*RoomSubscription
	for _, room := range b.subscriptions {
		for _, subscription := range room {
			if match(subscription) {
				targets = append(targets, subscription)
			}
		}
	}
	b.mu.RUnlock()
	for _, subscription := range targets {
		b.remove(subscription, closeInfo)
	}
}

func (b *InProcessRoomBus) DisconnectParticipant(participantID uuid.UUID,
	closeInfo RoomClose) {
	b.disconnectMatching(func(subscription *RoomSubscription) bool {
		return subscription.participantID == participantID
	}, closeInfo)
}

func (b *InProcessRoomBus) DisconnectProjectUser(projectID, userID uuid.UUID,
	closeInfo RoomClose) {
	b.disconnectMatching(func(subscription *RoomSubscription) bool {
		return subscription.projectID == projectID && subscription.userID == userID
	}, closeInfo)
}

func (b *InProcessRoomBus) DisconnectDevice(deviceID uuid.UUID,
	closeInfo RoomClose) {
	b.disconnectMatching(func(subscription *RoomSubscription) bool {
		return subscription.deviceID == deviceID
	}, closeInfo)
}

func (b *InProcessRoomBus) DisconnectDesktopSession(desktopSessionID uuid.UUID,
	closeInfo RoomClose) {
	b.disconnectMatching(func(subscription *RoomSubscription) bool {
		return subscription.desktopSessionID == desktopSessionID
	}, closeInfo)
}

func (b *InProcessRoomBus) DisconnectUser(userID uuid.UUID,
	closeInfo RoomClose) {
	b.disconnectMatching(func(subscription *RoomSubscription) bool {
		return subscription.userID == userID
	}, closeInfo)
}

func (b *InProcessRoomBus) ShutdownProject(projectID uuid.UUID,
	closeInfo RoomClose) {
	b.disconnectMatching(func(subscription *RoomSubscription) bool {
		return subscription.projectID == projectID
	}, closeInfo)
}

func (b *InProcessRoomBus) Shutdown(closeInfo RoomClose) {
	b.mu.Lock()
	b.closed = true
	var targets []*RoomSubscription
	for _, room := range b.subscriptions {
		for _, subscription := range room {
			targets = append(targets, subscription)
		}
	}
	b.subscriptions = make(map[uuid.UUID]map[uuid.UUID]*RoomSubscription)
	b.mu.Unlock()
	for _, subscription := range targets {
		subscription.markClosed(closeInfo)
	}
}
