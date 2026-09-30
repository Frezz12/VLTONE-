package collab

import (
	"context"
	"encoding/json"
	"github.com/google/uuid"
	"gorm.io/datatypes"
	"gorm.io/gorm"
	"vltstudio/backend/internal/model"
)

type PluginCatalog struct {
	CatalogRevision int64               `json:"catalogRevision"`
	PluginPolicy    string              `json:"pluginPolicy"`
	Plugins         []PluginRequirement `json:"plugins"`
}

func commonPluginInventory(session model.ProjectSession, members []model.ProjectSessionMember) ([]PluginRequirement, error) {
	var common []PluginRequirement
	editors := 0
	for _, member := range members {
		if !RoleAllows(member.EffectiveRole, PermissionEdit) || member.ReadinessStatus != model.SessionReadinessReady || member.ReadinessRevision != session.PluginRequirementsRevision || member.LeftAt != nil {
			continue
		}
		inventory, err := unmarshalPluginRequirements(json.RawMessage(member.PluginInventory))
		if err != nil {
			return nil, err
		}
		available := map[string]bool{}
		for _, plugin := range inventory {
			if session.PluginPolicy != "builtin_only" || plugin.Format == "internal" {
				available[pluginRequirementKey(plugin)] = true
			}
		}
		if editors == 0 {
			for _, plugin := range inventory {
				if available[pluginRequirementKey(plugin)] {
					common = append(common, plugin)
				}
			}
		} else {
			filtered := common[:0]
			for _, plugin := range common {
				if available[pluginRequirementKey(plugin)] {
					filtered = append(filtered, plugin)
				}
			}
			common = filtered
		}
		editors++
	}
	if common == nil {
		common = []PluginRequirement{}
	}
	return common, nil
}

func (s *Store) pluginCatalogTx(tx *gorm.DB, session model.ProjectSession) (PluginCatalog, error) {
	members, err := s.liveMembersTx(tx, session.ID)
	if err != nil {
		return PluginCatalog{}, err
	}
	plugins, err := commonPluginInventory(session, members)
	return PluginCatalog{CatalogRevision: session.CatalogRevision, PluginPolicy: session.PluginPolicy, Plugins: plugins}, err
}

func (s *Store) SessionPluginCatalog(ctx context.Context, projectID, sessionID, userID uuid.UUID) (PluginCatalog, error) {
	if _, err := s.GetProject(ctx, projectID, userID); err != nil {
		return PluginCatalog{}, err
	}
	session, err := s.openSessionTx(s.DB.WithContext(ctx), projectID, sessionID, false)
	if err != nil {
		return PluginCatalog{}, err
	}
	return s.pluginCatalogTx(s.DB.WithContext(ctx), session)
}

func (s *Store) UpdatePluginInventory(ctx context.Context, projectID, sessionID, userID, deviceID, desktopID uuid.UUID, inventory []PluginRequirement) (PluginCatalog, error) {
	var result PluginCatalog
	normalized, err := marshalPluginRequirements(inventory)
	if err != nil {
		return result, err
	}
	err = s.DB.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		if err := s.requireActiveActorTx(tx, userID, deviceID, desktopID); err != nil {
			return err
		}
		if _, err := s.projectAccess(tx, projectID, userID, true); err != nil {
			return err
		}
		session, err := s.openSessionTx(tx, projectID, sessionID, true)
		if err != nil {
			return err
		}
		if session.CommandSchemaVersion < CollaborationCommandSchemaV6 {
			return ErrVersionMismatch
		}
		member, err := s.activeSessionMemberTx(tx, sessionID, userID, deviceID, desktopID, true)
		if err != nil {
			return err
		}
		// Inventory must continue to cover the currently required project state.
		// A failed plugin should first be reported through readiness so this
		// participant stops being an editor before dropping it from inventory.
		if member.ReadinessStatus == model.SessionReadinessReady && RoleAllows(member.EffectiveRole, PermissionEdit) {
			required, err := unmarshalPluginRequirements(json.RawMessage(session.PluginRequirements))
			if err != nil {
				return err
			}
			available := map[string]bool{}
			for _, plugin := range inventory {
				available[pluginRequirementKey(plugin)] = true
			}
			for _, plugin := range required {
				if !available[pluginRequirementKey(plugin)] {
					return ErrPluginNotReady
				}
			}
		}
		if err := tx.Model(&member).Update("plugin_inventory", datatypes.JSON(normalized)).Error; err != nil {
			return err
		}
		if err := tx.Model(&session).Update("catalog_revision", gorm.Expr("catalog_revision + 1")).Error; err != nil {
			return err
		}
		session.CatalogRevision++
		result, err = s.pluginCatalogTx(tx, session)
		return err
	})
	return result, err
}

func validateV6Inventory(compatibility ClientCompatibility, requirements []PluginRequirement) error {
	if compatibility.PluginPolicy != "builtin_only" && compatibility.PluginPolicy != "external_checked" {
		return invalidf("invalid plugin policy")
	}
	available := map[string]bool{}
	for _, plugin := range compatibility.PluginInventory {
		available[pluginRequirementKey(plugin)] = true
	}
	for _, plugin := range requirements {
		if compatibility.PluginPolicy == "builtin_only" && plugin.Format != "internal" {
			return ErrPluginNotReady
		}
		if !available[pluginRequirementKey(plugin)] {
			return ErrPluginNotReady
		}
	}
	return nil
}

func CommandUsesExternalPlugins(kind string, payload json.RawMessage) bool {
	values, err := externalPluginRequirements(kind, payload)
	return err == nil && len(values) > 0
}

func extendPluginRequirementsTx(tx *gorm.DB, session model.ProjectSession, kind string, payload json.RawMessage) error {
	requested, err := externalPluginRequirements(kind, payload)
	if err != nil {
		return err
	}
	if len(requested) == 0 {
		return nil
	}
	required, err := unmarshalPluginRequirements(json.RawMessage(session.PluginRequirements))
	if err != nil {
		return err
	}
	seen := map[string]bool{}
	for _, plugin := range required {
		seen[pluginRequirementKey(plugin)] = true
	}
	changed := false
	for _, plugin := range requested {
		key := pluginRequirementKey(plugin)
		if !seen[key] {
			required = append(required, plugin)
			seen[key] = true
			changed = true
		}
	}
	if !changed {
		return nil
	}
	encoded, err := marshalPluginRequirements(required)
	if err != nil {
		return err
	}
	// Keep past requirements until the room ends: undo may restore an earlier
	// slot. Existing participant reports become stale and must be re-probed.
	return tx.Model(&session).Updates(map[string]any{"plugin_requirements": datatypes.JSON(encoded), "plugin_requirements_revision": gorm.Expr("plugin_requirements_revision + 1"), "catalog_revision": gorm.Expr("catalog_revision + 1")}).Error
}
