"use client";
import type { APIError, User } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { Search } from "lucide-react";
import Link from "next/link";
import { FormEvent, useEffect, useState } from "react";
import { AdminShell } from "./admin-shell";
import { CollaborationAccessSwitch } from "./collaboration-access-switch";
import { useAdmin } from "./use-admin";

export function UserRegistry() {
  const { session, error } = useAdmin();
  const [users, setUsers] = useState<User[]>([]);
  const [pending, setPending] = useState<Record<string, boolean>>({});
  const [accessErrors, setAccessErrors] = useState<Record<string, string>>({});
  const [accessSuccess, setAccessSuccess] = useState("");

  async function load(q = "") { const result = await api.request<{ users: User[] }>(`/v1/admin/users${q ? `?q=${encodeURIComponent(q)}` : ""}`); setUsers(result.users); }
  useEffect(() => { if (session) void load(); }, [session]);
  function search(event: FormEvent<HTMLFormElement>) { event.preventDefault(); const data = new FormData(event.currentTarget); void load(String(data.get("q") ?? "")); }

  async function setCollaborationAccess(user: User, enabled: boolean) {
    if (!session || pending[user.id]) return;
    setPending((current) => ({ ...current, [user.id]: true }));
    setAccessErrors((current) => ({ ...current, [user.id]: "" }));
    setAccessSuccess("");
    setUsers((current) => current.map((item) => item.id === user.id ? { ...item, collaboration_enabled: enabled } : item));
    try {
      const result = await api.json<{ collaboration_enabled: boolean }>(
        `/v1/admin/users/${user.id}/collaboration-access`, "PUT", { enabled }, session.csrf_token,
      );
      setUsers((current) => current.map((item) => item.id === user.id ? { ...item, collaboration_enabled: result.collaboration_enabled } : item));
      setAccessSuccess(`${user.nickname}: онлайн-доступ ${result.collaboration_enabled ? "включён" : "выключен"}.`);
    } catch (reason) {
      setUsers((current) => current.map((item) => item.id === user.id ? { ...item, collaboration_enabled: user.collaboration_enabled } : item));
      setAccessErrors((current) => ({ ...current, [user.id]: (reason as APIError).message || "Не удалось изменить онлайн-доступ." }));
    } finally {
      setPending((current) => ({ ...current, [user.id]: false }));
    }
  }

  return <AdminShell>
    <div className="admin-page-head">
      <div><h1 className="vlt-title">Пользователи</h1><p className="vlt-subtitle">Demo-аккаунты и состояние доступа.</p></div>
      <form className="vlt-row admin-search" onSubmit={search}><input className="vlt-input" name="q" placeholder="Email или никнейм" aria-label="Поиск пользователей" /><button className="vlt-button" aria-label="Искать"><Search size={16} /></button></form>
    </div>
    {error && <div className="vlt-error">{error}</div>}
    <span className="sr-only" role="status" aria-live="polite">{accessSuccess}</span>
    <div className="vlt-table-wrap"><table className="vlt-table">
      <thead><tr><th>Пользователь</th><th>Email</th><th>Статус</th><th>Онлайн-доступ</th><th>Создан</th><th /></tr></thead>
      <tbody>{users.map((user) => <tr key={user.id}>
        <td><strong>{user.nickname}</strong></td>
        <td>{user.email}</td>
        <td><span className="vlt-badge"><span className={`status-dot ${user.status !== "active" ? "off" : ""}`} />{user.status}</span></td>
        <td><CollaborationAccessSwitch
          enabled={user.collaboration_enabled}
          pending={Boolean(pending[user.id])}
          label={`Онлайн-доступ для ${user.nickname}`}
          error={accessErrors[user.id]}
          onChange={(enabled) => void setCollaborationAccess(user, enabled)}
        /></td>
        <td className="vlt-code">{new Date(user.created_at).toLocaleDateString("ru")}</td>
        <td><Link className="vlt-link" href={`/users/${user.id}`}>Открыть</Link></td>
      </tr>)}</tbody>
    </table></div>
  </AdminShell>;
}
