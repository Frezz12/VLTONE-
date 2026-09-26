"use client";

import type { APIError, AccountSession } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { legalReady, legalVersion } from "@/lib/legal";
import { readCookiePreference, requestCookieChoice } from "@/lib/cookie-preference";
import { Eye, EyeOff } from "lucide-react";
import Link from "next/link";
import { useTranslations } from "next-intl";
import { useRouter } from "next/navigation";
import { FormEvent, useEffect, useState } from "react";

function PasswordField({ label, name, autoComplete, showLabel, hideLabel }: {
  label: string;
  name: string;
  autoComplete: "current-password" | "new-password";
  showLabel: string;
  hideLabel: string;
}) {
  const [visible, setVisible] = useState(false);
  const actionLabel = visible ? hideLabel : showLabel;
  return <div className="vlt-label">
    <label htmlFor={name}>{label}</label>
    <div className="password-input">
      <input id={name} className="vlt-input" name={name} type={visible ? "text" : "password"} minLength={8} maxLength={128} autoComplete={autoComplete} required />
      <button className="password-toggle" type="button" onClick={() => setVisible((value) => !value)} aria-label={actionLabel} aria-pressed={visible} title={actionLabel}>
        {visible ? <EyeOff size={18} aria-hidden /> : <Eye size={18} aria-hidden />}
      </button>
    </div>
  </div>;
}

export function AuthForm({ locale, mode }: { locale: string; mode: "login" | "register" }) {
  const t = useTranslations("Auth");
  const router = useRouter();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [legalState, setLegalState] = useState<"loading" | "ready" | "unavailable">("loading");
  useEffect(() => {
    if (mode !== "register") return;
    const controller = new AbortController();
    api.request<{ registration_enabled?: boolean; registration_legal?: { version: string; ready: boolean } }>("/v1/meta", { signal: controller.signal })
      .then(value => setLegalState(value.registration_enabled && value.registration_legal?.version === legalVersion ? "ready" : "unavailable"))
      .catch(() => { if (!controller.signal.aborted) setLegalState("unavailable"); });
    return () => controller.abort();
  }, [mode]);
  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (busy || (mode === "register" && legalState !== "ready")) return;
    if (readCookiePreference() !== "necessary") {
      requestCookieChoice("account");
      setError(locale === "ru" ? "Для входа нужны необходимые cookie сессии. Разрешите их и повторите попытку." : "A necessary session cookie is required. Allow it and try again.");
      return;
    }
    setBusy(true); setError("");
    const form = new FormData(event.currentTarget);
    const payload = Object.fromEntries(form.entries());
    try {
      if (mode === "register") {
        if (form.get("password") !== form.get("password_confirmation")) { setError(t("passwordMismatch")); return; }
        Object.assign(payload, { locale,
          consent_accepted: form.get("consent_accepted") === "on", consent_version: legalVersion,
          terms_accepted: form.get("terms_accepted") === "on", terms_version: legalVersion,
          diagnostics_accepted: form.get("diagnostics_accepted") === "on", diagnostics_version: legalVersion,
        });
      }
      await api.json<AccountSession>(`/v1/web/auth/${mode === "login" ? "login" : "register"}`, "POST", payload);
      router.push("/account"); router.refresh();
    } catch (reason) {
      const failure = reason as APIError;
      const legalFailure = failure.code === "legal_documents_pending" || Object.keys(failure.field_errors ?? {}).some(key => ["consent_accepted", "terms_accepted", "diagnostics_accepted"].includes(key));
      if (legalFailure) { setLegalState("unavailable"); setError(t("legalUnavailable")); }
      else setError(failure.message ?? t("requestFailed"));
    }
    finally { setBusy(false); }
  }
  const register = mode === "register";
  return <main id="main-content" className="vlt-auth-wrap"><section className="vlt-card vlt-card-pad vlt-auth-card vlt-stack">
    <div><span className="section-label">VLTone</span><h1 className="vlt-title">{t(register ? "registerTitle" : "loginTitle")}</h1><p className="vlt-subtitle">{t(register ? "registerCopy" : "loginCopy")}</p></div>
    <form className="vlt-stack" onSubmit={submit}>
      <label className="vlt-label">{t("email")}<input className="vlt-input" name="email" type="email" autoComplete="email" required /></label>
      {register && <label className="vlt-label">{t("nickname")}<input className="vlt-input" name="nickname" minLength={3} maxLength={32} autoComplete="nickname" required /></label>}
      <PasswordField label={t("password")} name="password" autoComplete={register ? "new-password" : "current-password"} showLabel={t("showPassword")} hideLabel={t("hidePassword")} />
      {register && <PasswordField label={t("passwordAgain")} name="password_confirmation" autoComplete="new-password" showLabel={t("showPassword")} hideLabel={t("hidePassword")} />}
      {register && <div className="auth-legal">
        <label className="vlt-checkbox"><input name="terms_accepted" type="checkbox" required /><span>{t.rich("termsLabel", { terms: chunks => <Link href={`/terms?version=${legalVersion}`} target="_blank" rel="noopener noreferrer">{chunks}</Link> })}</span></label>
        <label className="vlt-checkbox"><input name="consent_accepted" type="checkbox" required /><span>{t.rich("consentLabel", { consent: chunks => <Link href={`/consent?version=${legalVersion}`} target="_blank" rel="noopener noreferrer">{chunks}</Link> })}</span></label>
        {legalReady && <label className="vlt-checkbox"><input name="diagnostics_accepted" type="checkbox" /><span>{t.rich("diagnosticsLabel", { diagnostics: chunks => <Link href={`/diagnostics-consent?version=${legalVersion}`} target="_blank" rel="noopener noreferrer">{chunks}</Link> })}</span></label>}
        <p className="auth-privacy-note">{t.rich("privacyNote", { privacy: chunks => <Link href="/privacy" target="_blank" rel="noopener noreferrer">{chunks}</Link> })}</p>
        {legalState !== "ready" && <p className="form-status" role="status">{t(legalState === "loading" ? "legalLoading" : "legalUnavailable")}</p>}
      </div>}
      {error && <div className="vlt-error" role="alert">{error}</div>}
      <button className="vlt-button" disabled={busy || (register && legalState !== "ready")}>{busy ? t("working") : t(register ? "register" : "login")}</button>
    </form>
    {!register && <Link className="vlt-link" href="/forgot-password">{t("forgot")}</Link>}
    <div className="auth-foot">{t(register ? "hasAccount" : "noAccount")} <Link className="vlt-link" href={register ? "/login" : "/register"}>{t(register ? "login" : "register")}</Link></div>
  </section></main>;
}
