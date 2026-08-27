"use client";

import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import Link from "next/link";
import { useSearchParams } from "next/navigation";
import { FormEvent, useState } from "react";

export function PasswordForm({ locale, confirm = false }: { locale: string; confirm?: boolean }) {
  const query = useSearchParams();
  const [message, setMessage] = useState("");
  const [error, setError] = useState("");
  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault(); setError(""); const form = new FormData(event.currentTarget);
    try {
      if (confirm) await api.json("/v1/web/auth/password-reset/confirm", "POST", { token: query.get("token"), password: form.get("password"), password_confirmation: form.get("password_confirmation") });
      else await api.json("/v1/web/auth/password-reset/request", "POST", { email: form.get("email") });
      setMessage(confirm ? (locale === "ru" ? "Пароль изменён. Теперь можно войти." : "Password changed. You can sign in now.") : (locale === "ru" ? "Если аккаунт существует, инструкция отправлена." : "If the account exists, instructions have been sent."));
    } catch (reason) { setError((reason as APIError).message); }
  }
  return <main className="vlt-auth-wrap"><section className="vlt-card vlt-card-pad vlt-auth-card vlt-stack"><h1 className="vlt-title">{confirm ? (locale === "ru" ? "Новый пароль" : "New password") : (locale === "ru" ? "Восстановление" : "Password reset")}</h1>{message && <div className="vlt-success">{message}</div>}{error && <div className="vlt-error">{error}</div>}<form className="vlt-stack" onSubmit={submit}>{confirm ? <><label className="vlt-label">{locale === "ru" ? "Пароль" : "Password"}<input className="vlt-input" name="password" type="password" minLength={12} maxLength={128} required /></label><label className="vlt-label">{locale === "ru" ? "Повторите пароль" : "Repeat password"}<input className="vlt-input" name="password_confirmation" type="password" minLength={12} maxLength={128} required /></label></> : <label className="vlt-label">Email<input className="vlt-input" name="email" type="email" required /></label>}<button className="vlt-button">{locale === "ru" ? "Продолжить" : "Continue"}</button></form><Link className="vlt-link" href={`/${locale}/login`}>{locale === "ru" ? "Вернуться ко входу" : "Back to sign in"}</Link></section></main>;
}
