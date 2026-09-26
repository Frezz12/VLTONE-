"use client";

import Link from "next/link";
import { useEffect, useState } from "react";
import { readCookiePreference, requestCookieChoice, saveCookiePreference, type CookiePromptReason } from "@/lib/cookie-preference";

export function CookieNotice({ locale }: { locale: string }) {
  const ru = locale === "ru";
  const [open, setOpen] = useState(false);
  const [reason, setReason] = useState<CookiePromptReason>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState(false);

  useEffect(() => {
    setOpen(readCookiePreference() === null);
    const request = (event: Event) => {
      setReason((event as CustomEvent<CookiePromptReason>).detail ?? null);
      setError(false);
      setOpen(true);
    };
    window.addEventListener("vlt-cookie-choice-request", request);
    return () => window.removeEventListener("vlt-cookie-choice-request", request);
  }, []);

  function accept() {
    saveCookiePreference("necessary");
    setOpen(false);
    setReason(null);
    const target = new URL(window.location.href);
    if (target.searchParams.get("lang") === locale) {
      document.cookie = `vlt-locale=${locale}; max-age=31536000; path=/; samesite=lax`;
      target.searchParams.delete("lang");
      window.location.assign(target);
    }
  }

  async function refuse() {
    setBusy(true);
    setError(false);
    try {
      const response = await fetch("/api/cookie-choice", { method: "POST", credentials: "same-origin" });
      if (!response.ok) throw new Error("Cookie removal failed");
      saveCookiePreference("none");
      setOpen(false);
      setReason(null);
      window.location.reload();
    } catch {
      setError(true);
      setBusy(false);
    }
  }

  if (!open) return null;
  return <section className="cookie-notice" aria-label={ru ? "Выбор cookie" : "Cookie choice"}>
    <div className="cookie-notice-copy">
      <span className="section-label">{ru ? "COOKIE НА САЙТЕ" : "SITE COOKIES"}</span>
      <h2>{reason ? (ru ? "Для этого нужны cookie" : "This needs cookies") : (ru ? "Ваш выбор" : "Your choice")}</h2>
      <p>{reason === "account"
          ? (ru ? "Для входа и регистрации нужна cookie сессии. Без неё публичные страницы доступны, но аккаунт работать не сможет." : "Sign-in and registration need a session cookie. Public pages remain available without it, but your account will not work.")
          : (ru ? "Cookie языка сохраняет ваш выбор, cookie сессии нужна при входе. Рекламных и аналитических cookie нет. Публичные страницы доступны без cookie, но язык не сохранится." : "A language cookie saves your choice; a session cookie is needed to sign in. There are no advertising or analytics cookies. Public pages work without cookies, but your language will not be saved.")}</p>
      <Link href="/privacy">{ru ? "Подробнее в политике данных" : "Read the privacy policy"}</Link>
      {error && <p className="cookie-notice-error" role="alert">{ru ? "Не удалось удалить cookie. Повторите попытку." : "Could not remove cookies. Please try again."}</p>}
    </div>
    <div className="cookie-notice-actions">
      <button type="button" className="cookie-accept" onClick={accept} disabled={busy}>{ru ? "Только необходимые" : "Necessary only"}</button>
      <button type="button" className="cookie-decline" onClick={refuse} disabled={busy}>{ru ? "Без cookie" : "No cookies"}</button>
    </div>
  </section>;
}

export function CookieSettingsButton({ locale }: { locale: string }) {
  return <button className="footer-cookie-settings" type="button" onClick={() => requestCookieChoice()}>{locale === "ru" ? "Настройки cookie" : "Cookie settings"}</button>;
}
