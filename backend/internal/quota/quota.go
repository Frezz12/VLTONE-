package quota

import (
	"errors"
	"fmt"
	"time"

	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"

	"vltstudio/backend/internal/model"
)

var (
	ErrExhausted       = errors.New("AI token quota exhausted")
	ErrGlobalExhausted = errors.New("global AI budget exhausted")
)

type Service struct {
	DB                 *gorm.DB
	GlobalMonthlyLimit int64
}

func Month(now time.Time) (time.Time, time.Time) {
	now = now.UTC()
	start := time.Date(now.Year(), now.Month(), 1, 0, 0, 0, 0, time.UTC)
	return start, start.AddDate(0, 1, 0)
}

func (s Service) Current(userID uuid.UUID, now time.Time) (model.TokenCycle, error) {
	start, end := Month(now)
	var cycle model.TokenCycle
	err := s.DB.Where("user_id = ? AND starts_at = ?", userID, start).First(&cycle).Error
	if err == nil {
		return cycle, nil
	}
	if !errors.Is(err, gorm.ErrRecordNotFound) {
		return cycle, err
	}
	cycle = model.TokenCycle{
		ID: uuid.New(), UserID: userID, StartsAt: start, EndsAt: end,
		BaseLimit: model.BaseDemoLimit,
	}
	if err := s.DB.Clauses(clause.OnConflict{DoNothing: true}).Create(&cycle).Error; err != nil {
		return cycle, err
	}
	err = s.DB.Where("user_id = ? AND starts_at = ?", userID, start).First(&cycle).Error
	return cycle, err
}

func Remaining(c model.TokenCycle) int64 {
	value := c.BaseLimit + c.Adjustment - c.UsedTokens - c.ReservedTokens
	if value < 0 {
		return 0
	}
	return value
}

func (s Service) Reserve(userID uuid.UUID, provider, modelName string, amount int64, now time.Time) (model.TokenReservation, error) {
	if amount <= 0 {
		return model.TokenReservation{}, errors.New("reservation must be positive")
	}
	var reservation model.TokenReservation
	err := s.DB.Transaction(func(tx *gorm.DB) error {
		if s.GlobalMonthlyLimit > 0 {
			if err := tx.Exec("SELECT pg_advisory_xact_lock(hashtext('vlt-global-ai-budget'))").Error; err != nil {
				return err
			}
			start, _ := Month(now)
			_, end := Month(now)
			var globalUsed int64
			// The global ceiling is based on immutable usage ledger entries plus
			// unsettled reservations. An administrator resetting a user's visible
			// cycle therefore cannot accidentally reopen the production budget.
			if err := tx.Raw(`
				SELECT COALESCE((SELECT SUM(-delta) FROM token_ledgers
				                  WHERE kind = 'usage' AND created_at >= ? AND created_at < ?), 0)
				     + COALESCE((SELECT SUM(reserved) FROM token_reservations
				                  WHERE settled_at IS NULL AND created_at >= ? AND created_at < ?), 0)`,
				start, end, start, end).Scan(&globalUsed).Error; err != nil {
				return err
			}
			if globalUsed+amount > s.GlobalMonthlyLimit {
				return ErrGlobalExhausted
			}
		}
		cycle, err := (Service{DB: tx, GlobalMonthlyLimit: s.GlobalMonthlyLimit}).Current(userID, now)
		if err != nil {
			return err
		}
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&cycle, "id = ?", cycle.ID).Error; err != nil {
			return err
		}
		if Remaining(cycle) < amount {
			return ErrExhausted
		}
		reservation = model.TokenReservation{
			ID: uuid.New(), CycleID: cycle.ID, UserID: userID,
			Provider: provider, Model: modelName, Reserved: amount,
		}
		if err := tx.Create(&reservation).Error; err != nil {
			return err
		}
		return tx.Model(&cycle).UpdateColumn("reserved_tokens", gorm.Expr("reserved_tokens + ?", amount)).Error
	})
	return reservation, err
}

func (s Service) Settle(reservationID uuid.UUID, actual int64, metadata datatypes.JSON, now time.Time) error {
	return s.settle(nil, reservationID, actual, metadata, now)
}

// SettleForUser prevents a desktop client from settling another account's
// reservation even if its unguessable identifier were disclosed.
func (s Service) SettleForUser(userID, reservationID uuid.UUID, actual int64, metadata datatypes.JSON, now time.Time) error {
	return s.settle(&userID, reservationID, actual, metadata, now)
}

func (s Service) settle(userID *uuid.UUID, reservationID uuid.UUID, actual int64, metadata datatypes.JSON, now time.Time) error {
	if actual < 0 {
		actual = 0
	}
	return s.DB.Transaction(func(tx *gorm.DB) error {
		var reservation model.TokenReservation
		query := tx.Clauses(clause.Locking{Strength: "UPDATE"}).Where("id = ?", reservationID)
		if userID != nil {
			query = query.Where("user_id = ?", *userID)
		}
		if err := query.First(&reservation).Error; err != nil {
			return err
		}
		if reservation.SettledAt != nil {
			return nil
		}
		// A compromised client must not be able to overcharge its account. The
		// reservation was conservatively sized before the credential was issued.
		if actual > reservation.Reserved {
			actual = reservation.Reserved
		}
		var cycle model.TokenCycle
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&cycle, "id = ?", reservation.CycleID).Error; err != nil {
			return err
		}
		cycle.ReservedTokens -= reservation.Reserved
		if cycle.ReservedTokens < 0 {
			cycle.ReservedTokens = 0
		}
		cycle.UsedTokens += actual
		if err := tx.Model(&cycle).Updates(map[string]any{
			"reserved_tokens": cycle.ReservedTokens,
			"used_tokens":     cycle.UsedTokens,
		}).Error; err != nil {
			return err
		}
		if err := tx.Model(&reservation).Update("settled_at", now.UTC()).Error; err != nil {
			return err
		}
		balance := cycle.BaseLimit + cycle.Adjustment - cycle.UsedTokens
		ledger := model.TokenLedger{
			ID: uuid.New(), CycleID: cycle.ID, UserID: cycle.UserID,
			ReservationID: &reservation.ID, Kind: "usage", Delta: -actual,
			BalanceAfter: balance, Metadata: metadata,
		}
		return tx.Create(&ledger).Error
	})
}

func (s Service) Adjust(userID, adminID uuid.UUID, amount int64, now time.Time) (model.TokenCycle, error) {
	if amount <= 0 {
		return model.TokenCycle{}, errors.New("adjustment must be positive")
	}
	cycle, err := s.Current(userID, now)
	if err != nil {
		return cycle, err
	}
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&cycle, "id = ?", cycle.ID).Error; err != nil {
			return err
		}
		cycle.Adjustment += amount
		if err := tx.Model(&cycle).Update("adjustment", cycle.Adjustment).Error; err != nil {
			return err
		}
		ledger := model.TokenLedger{
			ID: uuid.New(), CycleID: cycle.ID, UserID: userID, ActorAdminID: &adminID,
			Kind: "admin_adjustment", Delta: amount,
			BalanceAfter: cycle.BaseLimit + cycle.Adjustment - cycle.UsedTokens,
			Metadata:     datatypes.JSON([]byte(`{}`)),
		}
		return tx.Create(&ledger).Error
	})
	return cycle, err
}

func (s Service) Reset(userID, adminID uuid.UUID, now time.Time) (model.TokenCycle, error) {
	cycle, err := s.Current(userID, now)
	if err != nil {
		return cycle, err
	}
	err = s.DB.Transaction(func(tx *gorm.DB) error {
		if err := tx.Clauses(clause.Locking{Strength: "UPDATE"}).First(&cycle, "id = ?", cycle.ID).Error; err != nil {
			return err
		}
		if cycle.ReservedTokens != 0 {
			return fmt.Errorf("cannot reset while AI requests are active")
		}
		previous := cycle.UsedTokens
		cycle.UsedTokens = 0
		if err := tx.Model(&cycle).Update("used_tokens", 0).Error; err != nil {
			return err
		}
		ledger := model.TokenLedger{
			ID: uuid.New(), CycleID: cycle.ID, UserID: userID, ActorAdminID: &adminID,
			Kind: "admin_reset", Delta: previous,
			BalanceAfter: cycle.BaseLimit + cycle.Adjustment,
			Metadata:     datatypes.JSON([]byte(fmt.Sprintf(`{"previous_used":%d}`, previous))),
		}
		return tx.Create(&ledger).Error
	})
	return cycle, err
}
