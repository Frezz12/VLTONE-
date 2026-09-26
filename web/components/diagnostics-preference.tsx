"use client";

import { useState } from "react";
import Link from "next/link";
import { api, type User } from "@vlt/api-client";
import { legalVersion, legalReady } from "@/lib/legal";

export function DiagnosticsPreference({ user, csrf, locale }: { user: User; csrf: string; locale: string }) {
  const ru = locale === "ru";
  const [enabled, setEnabled] = useState(user.diagnostics_consent_version === legalVersion && Boolean(user.diagnostics_accepted_at) && !user.diagnostics_revoked_at);
  const [choice, setChoice] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [saved, setSaved] = useState(false);
  async function save() {
    if (busy || (!enabled && !choice)) return;
    setBusy(true); setError(""); setSaved(false);
    try {
      const result = await api.json<{ enabled: boolean }>("/v1/me/diagnostics-consent", "PUT", { accepted: !enabled, version: legalVersion }, csrf);
      setEnabled(result.enabled); setChoice(false); setSaved(true);
    } catch { setError(ru ? "Не удалось сохранить выбор. Попробуйте ещё раз." : "Your choice could not be saved. Please try again."); }
    finally { setBusy(false); }
  }
  return <section className="privacy-account"><h2>{ru ? "Диагностика" : "Diagnostics"}</h2>
    <p>{enabled ? (ru ? "Включена. Технические отчёты помогают исправлять ошибки. Можно отключить в любой момент." : "Enabled. Technical reports help fix errors. You can turn this off at any time.") : (ru ? "Выключена. Ваш аккаунт работает без необязательных диагностических отчётов." : "Disabled. Your account works without optional diagnostic reports.")}</p>
    {!enabled && legalReady && <label className="vlt-checkbox"><input type="checkbox" checked={choice} onChange={event => setChoice(event.target.checked)} /><span>{ru ? "Даю отдельное " : "I separately give "}<Link className="vlt-link" href={`/diagnostics-consent?version=${legalVersion}`} target="_blank" rel="noopener noreferrer">{ru ? "согласие на обработку диагностики" : "diagnostic data consent"}</Link>.</span></label>}
    {(enabled || legalReady) && <button type="button" className="vlt-button vlt-button-secondary" disabled={busy || (!enabled && !choice)} onClick={save}>{busy ? (ru ? "Сохраняем…" : "Saving…") : enabled ? (ru ? "Отозвать согласие и отключить" : "Withdraw consent and disable") : (ru ? "Включить диагностику" : "Enable diagnostics")}</button>}
    {error && <p role="alert">{error}</p>}{saved && <p role="status">{ru ? "Выбор сохранён." : "Preference saved."}</p>}
    <p><Link className="vlt-link" href="/privacy">{ru ? "Данные, сроки хранения и удаление аккаунта" : "Data, retention and account deletion"}</Link></p>
  </section>;
}
