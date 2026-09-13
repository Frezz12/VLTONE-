"use client";
import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import NextImage from "next/image";
import { useRouter } from "next/navigation";
import { FormEvent, useState } from "react";
import appIcon from "@/app/icon.png";

export function AdminLogin() {
  const router = useRouter(); const [error, setError] = useState(""); const [busy, setBusy] = useState(false);
  async function submit(event: FormEvent<HTMLFormElement>) { event.preventDefault(); setBusy(true); setError(""); const data = new FormData(event.currentTarget); try { await api.json("/v1/admin/auth/login", "POST", Object.fromEntries(data.entries())); router.replace("/"); router.refresh(); } catch (reason) { setError((reason as APIError).message); } finally { setBusy(false); } }
  return <main className="vlt-auth-wrap"><section className="vlt-card vlt-card-pad vlt-auth-card vlt-stack"><div className="vlt-brand admin-login-brand"><span className="admin-brand-icon"><NextImage src={appIcon} width={40} height={40} alt="" priority /></span><span className="admin-brand-copy"><strong>VLTONE</strong><small>Control center</small></span></div><div><span className="admin-auth-kicker">Закрытая панель</span><h1 className="vlt-title">Вход администратора</h1><p className="vlt-subtitle">Сессия завершится через 30 минут бездействия.</p></div><form className="vlt-stack" onSubmit={submit}><label className="vlt-label">Email<input className="vlt-input" name="email" type="email" autoComplete="email" required /></label><label className="vlt-label">Пароль<input className="vlt-input" name="password" type="password" autoComplete="current-password" required /></label>{error && <div className="vlt-error" role="alert">{error}</div>}<button className="vlt-button" disabled={busy}>{busy ? "Проверка…" : "Войти"}</button></form></section></main>;
}
