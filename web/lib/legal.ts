import profile from "../../backend/internal/legal/profile.json";

export const legalProfile = profile;
export const legalVersion = profile.version;
export const legalReady = Boolean(profile.operator_name && profile.operator_address && profile.contact_email && profile.database_location && profile.rkn_notification_confirmed && profile.processors_reviewed);
export const legalDocuments = ["terms", "privacy", "consent", "diagnostics-consent"] as const;
export type LegalDocument = typeof legalDocuments[number];
export const legalTitles = {
  ru: { terms: "Пользовательское соглашение", privacy: "Политика обработки персональных данных", consent: "Согласие на обработку персональных данных", "diagnostics-consent": "Согласие на обработку диагностики" },
  en: { terms: "Terms of use", privacy: "Personal data policy", consent: "Consent to personal data processing", "diagnostics-consent": "Diagnostic data consent" },
};
