"use client";

import type { APIError, AccountSession } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { legalReady, legalVersion } from "@/lib/legal";
import { readCookiePreference, requestCookieChoice } from "@/lib/cookie-preference";
import { ArrowLeft, ArrowRight, Eye, EyeOff, ShieldCheck } from "lucide-react";
import Link from "next/link";
import { useTranslations } from "next-intl";
import { useRouter } from "next/navigation";
import { FormEvent, useEffect, useState } from "react";
import { BrandMark } from "./brand-mark";
import { LocaleSwitch } from "./header";
import { AuthVisual } from "./auth-visual";
import { RegistrationCaptcha } from "./registration-captcha";

type CaptchaConfig = { provider: string; required: boolean; configured: boolean; site_key: string };

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
  const [captcha, setCaptcha] = useState<CaptchaConfig>();
  const [captchaToken, setCaptchaToken] = useState("");
  const [captchaReset, setCaptchaReset] = useState(0);
  const [metadataAttempt, setMetadataAttempt] = useState(0);
  useEffect(() => {
    if (mode !== "register") return;
    const controller = new AbortController();
    setLegalState("loading"); setCaptcha(undefined); setCaptchaToken("");
    api.request<{ registration_enabled?: boolean; registration_legal?: { version: string; ready: boolean }; registration_captcha?: CaptchaConfig }>("/v1/meta", { signal: controller.signal })
      .then(value => { if (controller.signal.aborted) return; setLegalState(value.registration_enabled && value.registration_legal?.version === legalVersion ? "ready" : "unavailable"); setCaptcha(value.registration_captcha); })
      .catch(() => { if (!controller.signal.aborted) setLegalState("unavailable"); });
    return () => controller.abort();
  }, [mode, metadataAttempt]);
  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (busy || (mode === "register" && legalState !== "ready")) return;
    if (mode === "register" && captcha?.required !== false && !captchaToken) return;
    if (readCookiePreference() !== "necessary") {
      requestCookieChoice("account");
      setError(locale === "ru" ? "Для входа нужны необходимые cookie сессии. Разрешите их и повторите попытку." : "A necessary session cookie is required. Allow it and try again.");
      return;
    }
    const form = new FormData(event.currentTarget);
    const payload = Object.fromEntries(form.entries());
    if (mode === "register" && form.get("password") !== form.get("password_confirmation")) { setError(t("passwordMismatch")); return; }
    setBusy(true); setError("");
    try {
      if (mode === "register") {
        Object.assign(payload, { locale,
          consent_accepted: form.get("consent_accepted") === "on", consent_version: legalVersion,
          terms_accepted: form.get("terms_accepted") === "on", terms_version: legalVersion,
          diagnostics_accepted: form.get("diagnostics_accepted") === "on", diagnostics_version: legalVersion,
        });
        if (captcha?.required !== false) payload.captcha_token = captchaToken;
      }
      await api.json<AccountSession>(`/v1/web/auth/${mode === "login" ? "login" : "register"}`, "POST", payload);
      document.cookie = `vlt-locale=${locale}; max-age=31536000; path=/; samesite=lax`;
      router.push("/account"); router.refresh();
    } catch (reason) {
      const failure = reason as APIError;
      const legalFailure = failure.code === "legal_documents_pending" || Object.keys(failure.field_errors ?? {}).some(key => ["consent_accepted", "terms_accepted", "diagnostics_accepted"].includes(key));
      if (legalFailure) { setLegalState("unavailable"); setError(t("legalUnavailable")); }
      else if (failure.code === "captcha_invalid") setError(t("captchaExpired"));
      else if (failure.code === "captcha_unavailable") setError(t("captchaUnavailable"));
      else setError(failure.message ?? t("requestFailed"));
    }
    finally { setBusy(false); if (mode === "register") { setCaptchaToken(""); setCaptchaReset(value => value + 1); } }
  }
  const register = mode === "register";
  return <main id="main-content" className="public-auth" data-mode={mode}>
    <aside className="auth-story"><div className="auth-story-content"><header className="auth-story-header"><Link className="vlt-brand auth-brand" href="/" aria-label="VLTone"><BrandMark /><span>VLTone</span></Link><LocaleSwitch locale={locale} className="auth-language" /></header><div className="auth-story-copy"><h2>{t("storyTitle")}</h2><p>{t("storyCopy")}</p></div><AuthVisual locale={locale} /></div></aside>
    <section className="auth-form-panel"><div className="auth-form-content"><Link className="auth-back" href="/"><ArrowLeft size={16} aria-hidden />{t("backToSite")}</Link>
    <div className="auth-form-heading"><span className="auth-kicker">{t(register ? "registerKicker" : "loginKicker")}</span><h1 className="vlt-title">{t(register ? "registerTitle" : "loginTitle")}</h1><p className="vlt-subtitle">{t(register ? "registerCopy" : "loginCopy")}</p></div>
    <form className="vlt-stack" onSubmit={submit}>
      <label className="vlt-label">{t("email")}<input className="vlt-input" name="email" type="email" autoComplete={register ? "email" : "username"} required /></label>
      {register && <label className="vlt-label">{t("nickname")}<input className="vlt-input" name="nickname" minLength={3} maxLength={32} autoComplete="nickname" required /></label>}
      <PasswordField label={t("password")} name="password" autoComplete={register ? "new-password" : "current-password"} showLabel={t("showPassword")} hideLabel={t("hidePassword")} />
      {!register && <Link className="vlt-link auth-forgot" href="/forgot-password">{t("forgot")}</Link>}
      {register && <PasswordField label={t("passwordAgain")} name="password_confirmation" autoComplete="new-password" showLabel={t("showPassword")} hideLabel={t("hidePassword")} />}
      {register && <div className="auth-legal">
        <label className="vlt-checkbox"><input name="terms_accepted" type="checkbox" required /><span>{t.rich("termsLabel", { terms: chunks => <Link href={`/terms?version=${legalVersion}`} target="_blank" rel="noopener noreferrer">{chunks}</Link> })}</span></label>
        <label className="vlt-checkbox"><input name="consent_accepted" type="checkbox" required /><span>{t.rich("consentLabel", { consent: chunks => <Link href={`/consent?version=${legalVersion}`} target="_blank" rel="noopener noreferrer">{chunks}</Link> })}</span></label>
        {legalReady && <label className="vlt-checkbox"><input name="diagnostics_accepted" type="checkbox" /><span>{t.rich("diagnosticsLabel", { diagnostics: chunks => <Link href={`/diagnostics-consent?version=${legalVersion}`} target="_blank" rel="noopener noreferrer">{chunks}</Link> })}</span></label>}
        <p className="auth-privacy-note">{t.rich("privacyNote", { privacy: chunks => <Link href="/privacy" target="_blank" rel="noopener noreferrer">{chunks}</Link> })}</p>
        {legalState !== "ready" && <p className="form-status" role="status">{t(legalState === "loading" ? "legalLoading" : "legalUnavailable")}</p>}
      </div>}
      {register && legalState === "ready" && (captcha?.required === false ? null : captcha?.provider === "turnstile" && captcha.configured && captcha.site_key ? <RegistrationCaptcha siteKey={captcha.site_key} locale={locale} reset={captchaReset} onToken={setCaptchaToken} /> : <div className="auth-captcha auth-captcha-unavailable"><span className="auth-captcha-label">{t("captchaLabel")}</span><p role="status">{t("captchaUnavailable")}</p><button type="button" onClick={() => setMetadataAttempt(value => value + 1)}>{t("captchaRetry")}</button></div>)}
      {error && <div className="vlt-error" role="alert">{error}</div>}
      <button className="vlt-button auth-submit" disabled={busy || (register && (legalState !== "ready" || (captcha?.required !== false && !captchaToken)))}>{busy ? t("working") : t(register ? "register" : "login")}<ArrowRight size={18} aria-hidden /></button>
    </form>
    <div className="auth-foot">{t(register ? "hasAccount" : "noAccount")} <Link className="vlt-link" href={register ? "/login" : "/register"}>{t(register ? "login" : "register")}</Link></div>
    <footer className="auth-trust"><ShieldCheck size={16} aria-hidden /><span>{t("accountHint")}</span></footer>
  </div></section></main>;
}
