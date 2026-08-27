"use client";

import type { APIError, AccountSession } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { Paperclip, Send } from "lucide-react";
import { useTranslations } from "next-intl";
import { useRouter } from "next/navigation";
import { FormEvent, useEffect, useState } from "react";

export function BugReportForm({ locale }: { locale: string }) {
  const t = useTranslations("Bug");
  const router = useRouter();
  const [session, setSession] = useState<AccountSession>();
  const [error, setError] = useState("");
  const [receipt, setReceipt] = useState<number>();
  const [busy, setBusy] = useState(false);
  useEffect(() => { api.request<AccountSession>("/v1/me").then(setSession).catch(() => router.replace(`/${locale}/login`)); }, []);
  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault(); if (!session) return;
    const formElement = event.currentTarget;
    const form = new FormData(formElement);
    const files = form.getAll("attachments").filter((item): item is File => typeof item !== "string" && item.size > 0);
    if (files.length > 5) { setError(t("tooManyAttachments")); return; }
    if (files.some((file) => file.size > 10 * 1024 * 1024 || !["image/jpeg", "image/png", "image/webp"].includes(file.type))) {
      setError(t("invalidAttachment")); return;
    }
    setBusy(true); setError("");
    try {
      const response = await api.request<{ number: number }>("/v1/bug-reports", { method: "POST", headers: { "X-CSRF-Token": session.csrf_token }, body: form });
      setReceipt(response.number); formElement.reset();
    } catch (reason) { setError((reason as APIError).message); }
    finally { setBusy(false); }
  }
  return <main className="vlt-main"><section className="vlt-card vlt-card-pad vlt-stack report-card">
    <div><h1 className="vlt-title">{t("title")}</h1><p className="vlt-subtitle">{t("subtitle")}</p></div>
    {receipt && <div className="vlt-success" role="status">{t("sent", { number: receipt })}</div>}
    {error && <div className="vlt-error" role="alert">{error}</div>}
    <form className="vlt-stack" onSubmit={submit}>
      <label className="vlt-label">{t("subject")}<input className="vlt-input" name="title" minLength={3} maxLength={160} required /></label>
      <label className="vlt-label">{t("description")}<textarea className="vlt-input" name="description" minLength={10} maxLength={20000} required /></label>
      <label className="vlt-label">{t("steps")}<textarea className="vlt-input" name="steps" /></label>
      <div className="vlt-grid vlt-grid-2"><label className="vlt-label">{t("expected")}<textarea className="vlt-input" name="expected" /></label><label className="vlt-label">{t("actual")}<textarea className="vlt-input" name="actual" /></label></div>
      <label className="vlt-label"><span className="vlt-row"><Paperclip size={15} aria-hidden /> {t("attachments")}</span><input className="vlt-input" name="attachments" type="file" accept="image/jpeg,image/png,image/webp" multiple /></label>
      <div className="form-actions"><button className="vlt-button" disabled={!session || busy}><Send size={16} aria-hidden /> {t("send")}</button></div>
    </form>
  </section></main>;
}
