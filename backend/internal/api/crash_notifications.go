package api

import (
	"fmt"
	"log"
	"strings"

	"vltstudio/backend/internal/model"
)

// sendCrashNotification runs after the report has been committed. SMTP is
// already a required production dependency, so crash alerts do not need a
// second delivery service or a second set of credentials.
func (s *Server) sendCrashNotification(report model.CrashReport) {
	if s.Config.SMTPHost == "" {
		if s.Config.Environment == "development" {
			log.Printf("development crash notification: report=%s reason=%q", report.ID, report.Reason)
		}
		return
	}
	var admins []model.AdminUser
	if err := s.DB.Where("status = ?", model.UserActive).Find(&admins).Error; err != nil {
		log.Printf("load crash notification recipients: %v", err)
		return
	}
	link := strings.TrimRight(s.Config.AdminOrigin, "/") + "/crashes"
	body := fmt.Sprintf(
		"A new VLT Studio Pro crash report was received.\r\n\r\n"+
			"Occurred: %s\r\nVersion: %s\r\nBuild: %s\r\nPlatform: %s\r\n"+
			"Reason: %s\r\nLast plugin: %s\r\nReport ID: %s\r\n\r\nOpen: %s\r\n",
		report.OccurredAt.UTC().Format("2006-01-02 15:04:05.000 MST"),
		report.AppVersion, report.BuildID, report.Platform, report.Reason,
		report.LastPlugin, report.ID, link)
	for _, admin := range admins {
		if err := s.sendPlainEmail(admin.Email, "VLT Studio: new crash report", body); err != nil {
			log.Printf("send crash notification to %s: %v", admin.ID, err)
		}
	}
}
