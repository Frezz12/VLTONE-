"use client";
import { api, type APIError } from "@vlt/api-client";
import { useEffect, useState, type FormEvent } from "react";
import { ShieldCheck, Plus, Users } from "lucide-react";
import { AdminShell } from "./admin-shell";
import { useAdmin } from "./use-admin";
import { permissionSections, type AdminIdentity } from "./admin-permissions";

export function AdminTeam() {
  const { session, error: authError } = useAdmin();
  const [members, setMembers] = useState<AdminIdentity[]>([]);
  const [editing, setEditing] = useState<AdminIdentity>();
  const [email, setEmail] = useState("");
  const [permissions, setPermissions] = useState<string[]>(["tasks.read", "tasks.write"]);
  const [enabled, setEnabled] = useState(true);
  const [busy, setBusy] = useState(false);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  async function load() { try { const result = await api.request<{ members: AdminIdentity[] }>("/v1/admin/team"); setMembers(result.members); } catch (e) { setError((e as APIError).message); } finally { setLoading(false); } }
  useEffect(() => { if (session?.admin.is_owner) void load(); }, [session]);
  function select(member?: AdminIdentity) { setEditing(member); setEmail(member?.email ?? ""); setPermissions(member?.permissions ?? ["tasks.read", "tasks.write"]); setEnabled(member ? member.status === "active" : true); setError(""); setNotice(""); }
  function toggle(section: string, kind: string, checked: boolean) {
    setPermissions(current => { const next = new Set(current); if (checked) { next.add(`${section}.${kind}`); next.add(`${section}.read`); } else { next.delete(`${section}.${kind}`); if (kind === "read") next.delete(`${section}.write`); } return [...next]; });
  }
  async function save(event: FormEvent) {
    event.preventDefault(); if (!session || busy) return; setBusy(true); setError(""); setNotice("");
    try { await api.json("/v1/admin/team", "PUT", { email, permissions, enabled }, session.csrf_token); setNotice(enabled ? "Доступ сохранён. Сотрудник может войти с паролем от сайта." : "Доступ отозван. Сессии сотрудника завершены."); await load(); }
    catch (e) { setError((e as APIError).message); } finally { setBusy(false); }
  }
  return <AdminShell><div className="admin-page-head"><div><h1 className="vlt-title">Команда</h1><p className="vlt-subtitle">Аккаунты сайта, которым вы доверяете работу в админке.</p></div><button className="vlt-button vlt-button-secondary" onClick={() => select()} disabled={busy}><Plus size={16} />Добавить участника</button></div>
    {(error || authError) && <p className="vlt-error" role="alert">{error || authError}</p>}{notice && <p className="team-notice" role="status">{notice}</p>}
    <div className="team-layout"><section className="vlt-card vlt-card-pad team-members" aria-label="Участники команды"><h2><Users size={18} /> Участники</h2>{loading ? <p role="status">Загрузка…</p> : members.map(member => <button key={member.id} className="team-member" disabled={member.is_owner || busy || !member.user_id} onClick={() => select(member)} aria-pressed={editing?.id === member.id}><span><strong>{member.nickname}</strong><small>{member.email}</small></span><span className="vlt-badge">{member.is_owner ? "Владелец" : member.status === "active" ? "Есть доступ" : "Доступ отозван"}</span></button>)}</section>
    <form className="vlt-card vlt-card-pad vlt-stack" onSubmit={save}><h2>{editing ? `Права: ${editing.nickname}` : "Новый участник"}</h2><label className="vlt-label">Email аккаунта сайта<input className="vlt-input" type="email" required value={email} disabled={busy || !!editing} onChange={e => setEmail(e.target.value)} placeholder="name@example.com" /></label><p className="vlt-muted">Аккаунт должен быть уже зарегистрирован. Отдельный пароль не нужен.</p>
      <label className="team-check"><input type="checkbox" checked={enabled} disabled={busy} onChange={e => setEnabled(e.target.checked)} />Разрешить вход в админку</label>
      <fieldset disabled={busy}><legend>Права по разделам</legend><div className="team-permission-head"><span>Раздел</span><span>Просмотр</span><span>Изменение</span></div>{permissionSections.map(([key, label, writable]) => <div className="team-permission" key={key}><span>{label}</span><label><span className="sr-only">{label}: просмотр</span><input type="checkbox" checked={permissions.includes(`${key}.read`)} onChange={e => toggle(key, "read", e.target.checked)} /></label>{writable ? <label><span className="sr-only">{label}: изменение</span><input type="checkbox" checked={permissions.includes(`${key}.write`)} onChange={e => toggle(key, "write", e.target.checked)} /></label> : <span aria-label="Недоступно">—</span>}</div>)}</fieldset>
      <p className="vlt-muted"><ShieldCheck size={14} /> Управление командой доступно только владельцу. Изменение прав завершает текущие сессии участника.</p><p className="vlt-muted">«Изменение» включает создание, редактирование и удаление в разделе. Для пользователей — также блокировку аккаунтов, управление токенами и сессиями.</p><button className="vlt-button" disabled={busy || !email.trim()}>{busy ? "Сохраняем…" : editing ? "Сохранить доступ" : "Предоставить доступ"}</button>
    </form></div></AdminShell>;
}
