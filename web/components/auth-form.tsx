"use client";

import type { APIError, AccountSession } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import Link from "next/link";
import { useTranslations } from "next-intl";
import { useRouter } from "next/navigation";
import { FormEvent, useEffect, useState } from "react";

export function AuthForm({ locale, mode }: { locale: string; mode: "login" | "register" }) {
  const t = useTranslations("Auth");
  const router = useRouter();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [consentVersion, setConsentVersion] = useState("2026-08-23");
  useEffect(() => { api.request<{ consent_version: string }>("/v1/meta").then((value) => setConsentVersion(value.consent_version)).catch(() => undefined); }, []);
  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault(); setBusy(true); setError("");
    const form = new FormData(event.currentTarget);
    const payload = Object.fromEntries(form.entries());
    try {
      if (mode === "register") {
        Object.assign(payload, { locale, consent_accepted: form.get("consent_accepted") === "on", consent_version: consentVersion });
      }
      await api.json<AccountSession>(`/v1/web/auth/${mode === "login" ? "login" : "register"}`, "POST", payload);
      router.push(`/${locale}/account`); router.refresh();
    } catch (reason) { setError((reason as APIError).message ?? "Request failed"); }
    finally { setBusy(false); }
  }
  const register = mode === "register";
  return <main className="vlt-auth-wrap"><section className="vlt-card vlt-card-pad vlt-auth-card vlt-stack">
    <div><h1 className="vlt-title">{t(register ? "registerTitle" : "loginTitle")}</h1></div>
    <form className="vlt-stack" onSubmit={submit}>
      <label className="vlt-label">{t("email")}<input className="vlt-input" name="email" type="email" autoComplete="email" required /></label>
      {register && <label className="vlt-label">{t("nickname")}<input className="vlt-input" name="nickname" minLength={3} maxLength={32} autoComplete="nickname" required /></label>}
      <label className="vlt-label">{t("password")}<input className="vlt-input" name="password" type="password" minLength={12} maxLength={128} autoComplete={register ? "new-password" : "current-password"} required /></label>
      {register && <label className="vlt-label">{t("passwordAgain")}<input className="vlt-input" name="password_confirmation" type="password" minLength={12} maxLength={128} autoComplete="new-password" required /></label>}
      {register && <label className="vlt-checkbox"><input name="consent_accepted" type="checkbox" required /><span>{t("consent", { version: consentVersion })}</span></label>}
      {error && <div className="vlt-error" role="alert">{error}</div>}
      <button className="vlt-button" disabled={busy}>{busy ? t("working") : t(register ? "register" : "login")}</button>
    </form>
    {!register && <Link className="vlt-link" href={`/${locale}/forgot-password`}>{t("forgot")}</Link>}
    <div className="auth-foot">{t(register ? "hasAccount" : "noAccount")} <Link className="vlt-link" href={`/${locale}/${register ? "login" : "register"}`}>{t(register ? "login" : "register")}</Link></div>
  </section></main>;
}
