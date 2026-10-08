"use client";
import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import NextImage from "next/image";
import { ArrowRight, ShieldCheck } from "lucide-react";
import { useRouter } from "next/navigation";
import { FormEvent, useState } from "react";
import appIcon from "@/app/icon.png";

export function AdminLogin() {
  const router = useRouter(); const [error, setError] = useState(""); const [busy, setBusy] = useState(false);
  async function submit(event: FormEvent<HTMLFormElement>) { event.preventDefault(); setBusy(true); setError(""); const data = new FormData(event.currentTarget); try { await api.json("/v1/admin/auth/login", "POST", Object.fromEntries(data.entries())); router.replace("/"); router.refresh(); } catch (reason) { setError((reason as APIError).message); } finally { setBusy(false); } }
  return <main className="admin-login">
    <section className="admin-login-story" aria-label="VLTone"><div className="vlt-brand"><span className="admin-brand-icon"><NextImage src={appIcon} width={40} height={40} alt="" priority /></span><span className="admin-brand-copy"><strong>VLTone</strong><small>Панель управления</small></span></div><div><h2>Всё, что стоит за музыкой.</h2><p>Новые релизы, работа сервиса и обратная связь от тех, кто создаёт музыку в VLTone.</p></div><svg className="admin-login-wave" viewBox="0 0 480 140" fill="none" aria-hidden><path d="M0 70h28l8-18 10 36 12-58 14 80 16-112 18 144 18-104 12 60 14-40 10 12h36l12-26 12 54 14-84 16 114 18-142 18 126 14-78 14 38 12-2h28l12-20 12 42 14-70 14 98 18-124 16 100 14-52 12 28h40" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round" /></svg></section>
    <section className="admin-login-form"><div><span className="admin-auth-kicker">Добро пожаловать</span><h1 className="vlt-title">Вход администратора</h1><p className="vlt-subtitle">Войдите с рабочим аккаунтом, чтобы продолжить.</p></div><form className="vlt-stack" onSubmit={submit}><label className="vlt-label">Email<input className="vlt-input" name="email" type="email" autoComplete="username" required /></label><label className="vlt-label">Пароль<input className="vlt-input" name="password" type="password" autoComplete="current-password" required /></label>{error && <div className="vlt-error" role="alert">{error}</div>}<button className="vlt-button" disabled={busy}>{busy ? "Проверка…" : "Войти"}<ArrowRight size={17} aria-hidden /></button></form><footer><ShieldCheck size={16} aria-hidden /><span>Сессия завершится через 30 минут бездействия.</span></footer></section>
  </main>;
}
