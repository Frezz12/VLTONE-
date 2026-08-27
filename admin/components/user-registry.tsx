"use client";
import type { User } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { Search } from "lucide-react";
import Link from "next/link";
import { FormEvent, useEffect, useState } from "react";
import { AdminShell } from "./admin-shell";
import { useAdmin } from "./use-admin";

export function UserRegistry() {
  const { session, error } = useAdmin(); const [users, setUsers] = useState<User[]>([]);
  async function load(q = "") { const result = await api.request<{ users: User[] }>(`/v1/admin/users${q ? `?q=${encodeURIComponent(q)}` : ""}`); setUsers(result.users); }
  useEffect(() => { if (session) void load(); }, [session]);
  function search(event: FormEvent<HTMLFormElement>) { event.preventDefault(); const data = new FormData(event.currentTarget); void load(String(data.get("q") ?? "")); }
  return <AdminShell><div className="admin-page-head"><div><h1 className="vlt-title">Пользователи</h1><p className="vlt-subtitle">Demo-аккаунты и состояние доступа.</p></div><form className="vlt-row admin-search" onSubmit={search}><input className="vlt-input" name="q" placeholder="Email или никнейм" aria-label="Поиск пользователей" /><button className="vlt-button" aria-label="Искать"><Search size={16} /></button></form></div>{error && <div className="vlt-error">{error}</div>}<div className="vlt-table-wrap"><table className="vlt-table"><thead><tr><th>Пользователь</th><th>Email</th><th>Статус</th><th>Создан</th><th /></tr></thead><tbody>{users.map((user) => <tr key={user.id}><td><strong>{user.nickname}</strong></td><td>{user.email}</td><td><span className="vlt-badge"><span className={`status-dot ${user.status !== "active" ? "off" : ""}`} />{user.status}</span></td><td className="vlt-code">{new Date(user.created_at).toLocaleDateString("ru")}</td><td><Link className="vlt-link" href={`/users/${user.id}`}>Открыть</Link></td></tr>)}</tbody></table></div></AdminShell>;
}
