"use client";
import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { useRouter } from "next/navigation";
import { FormEvent, useState } from "react";

export function AdminLogin() {
  const router = useRouter(); const [error, setError] = useState(""); const [busy, setBusy] = useState(false);
  async function submit(event: FormEvent<HTMLFormElement>) { event.preventDefault(); setBusy(true); setError(""); const data = new FormData(event.currentTarget); try { await api.json("/v1/admin/auth/login", "POST", Object.fromEntries(data.entries())); router.replace("/"); router.refresh(); } catch (reason) { setError((reason as APIError).message); } finally { setBusy(false); } }
  return <main className="vlt-auth-wrap"><section className="vlt-card vlt-card-pad vlt-auth-card vlt-stack"><div className="vlt-brand"><span className="vlt-brand-mark">VLT</span><span>Control</span></div><div><h1 className="vlt-title">Вход администратора</h1><p className="vlt-subtitle">Сессия: 30 минут простоя, максимум 8 часов.</p></div><form className="vlt-stack" onSubmit={submit}><label className="vlt-label">Email<input className="vlt-input" name="email" type="email" autoComplete="email" required /></label><label className="vlt-label">Пароль<input className="vlt-input" name="password" type="password" autoComplete="current-password" required /></label>{error && <div className="vlt-error">{error}</div>}<button className="vlt-button" disabled={busy}>{busy ? "Проверка…" : "Войти"}</button></form></section></main>;
}
