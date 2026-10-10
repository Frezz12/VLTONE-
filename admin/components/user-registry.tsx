"use client";
import type { APIError, User } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { Download, Search } from "lucide-react";
import { useSearchParams } from "next/navigation";
import Link from "next/link";
import { FormEvent, useEffect, useState } from "react";
import { AdminShell } from "./admin-shell";
import { CollaborationAccessSwitch } from "./collaboration-access-switch";
import { canAdmin } from "./admin-permissions";
import { useAdmin } from "./use-admin";
import { downloadCsv } from "./export-csv";

export function UserRegistry() {
  const params = useSearchParams();
  const urlQuery = params.get("q") ?? "";
  const { session, error } = useAdmin();
  const canWrite = canAdmin(session?.admin, "users.write");
  const [users, setUsers] = useState<User[]>([]);
  const [pending, setPending] = useState<Record<string, boolean>>({});
  const [accessErrors, setAccessErrors] = useState<Record<string, string>>({});
  const [accessSuccess, setAccessSuccess] = useState("");
  const [query, setQuery] = useState("");
  const [statusFilter, setStatusFilter] = useState("");
  const [loadError, setLoadError] = useState("");
  const [loading, setLoading] = useState(true);

  async function load(q = "") {
    setLoading(true); setLoadError("");
    try { const result = await api.request<{ users: User[] }>(`/v1/admin/users${q ? `?q=${encodeURIComponent(q)}` : ""}`); setUsers(result.users); }
    catch { setLoadError("Не удалось загрузить пользователей. Повторите поиск."); }
    finally { setLoading(false); }
  }
  useEffect(() => { if (session) { setQuery(urlQuery); void load(urlQuery); } }, [session, urlQuery]);
  function search(event: FormEvent<HTMLFormElement>) { event.preventDefault(); void load(query.trim()); }
  const visibleUsers = users.filter(user => !statusFilter || user.status === statusFilter);

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
      <div><h1 className="vlt-title">Пользователи</h1><p className="vlt-subtitle">Аккаунты, состояние доступа и история активности.</p></div>
      <button className="vlt-button vlt-button-secondary" disabled={loading || !visibleUsers.length} onClick={() => downloadCsv("vltone-users.csv", ["Пользователь", "Email", "Статус", "Онлайн-доступ", "Создан"], visibleUsers.map(user => [user.nickname, user.email, user.status, user.collaboration_enabled ? "Включён" : "Выключен", user.created_at]))}><Download size={16} aria-hidden />Выгрузить CSV</button>
    </div>
    {(error || loadError) && <div className="vlt-error" role="alert">{error || loadError}</div>}
    <div className="admin-list-toolbar"><form className="vlt-row admin-search" onSubmit={search}><input className="vlt-input" type="search" name="q" placeholder="Email или никнейм" aria-label="Поиск пользователей" value={query} onChange={event => setQuery(event.target.value)} /><button className="vlt-button" aria-label="Искать" disabled={loading}><Search size={16} aria-hidden /></button></form><label className="admin-inline-filter">Статус<select className="vlt-input" value={statusFilter} onChange={event => setStatusFilter(event.target.value)}><option value="">Все аккаунты</option><option value="active">Активные</option><option value="suspended">Приостановленные</option></select></label><span className="admin-result-count" aria-live="polite">{loading ? "Загрузка…" : `В списке: ${visibleUsers.length}`}</span></div>
    <span className="sr-only" role="status" aria-live="polite">{accessSuccess}</span>
    <div className="vlt-table-wrap"><table className="vlt-table">
      <thead><tr><th>Пользователь</th><th>Email</th><th>Статус</th><th>Онлайн-доступ</th><th>Создан</th><th /></tr></thead>
      <tbody>{visibleUsers.map((user) => <tr key={user.id}>
        <td><strong>{user.nickname}</strong></td>
        <td>{user.email}</td>
        <td><span className="vlt-badge"><span className={`status-dot ${user.status !== "active" ? "off" : ""}`} />{user.status}</span></td>
        <td><CollaborationAccessSwitch
          disabled={!canWrite}
          enabled={user.collaboration_enabled}
          pending={Boolean(pending[user.id])}
          label={`Онлайн-доступ для ${user.nickname}`}
          error={accessErrors[user.id]}
          onChange={(enabled) => void setCollaborationAccess(user, enabled)}
        /></td>
        <td className="vlt-code">{new Date(user.created_at).toLocaleDateString("ru")}</td>
        <td><Link className="vlt-link" href={`/users/${user.id}`}>Открыть</Link></td>
      </tr>)}
      {!visibleUsers.length && !loading && <tr><td colSpan={6} className="admin-empty">Пользователи не найдены. Измените поиск или статус.</td></tr>}
    </tbody></table></div>
  </AdminShell>;
}
