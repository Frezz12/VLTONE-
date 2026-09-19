"use client";

import Link from "next/link";
import { useState } from "react";

type UsageUser = { user_id: string; nickname: string; sessions: number; total_seconds: number; last_started_at?: string; last_seen_at?: string };
type Launch = { id: string; user_id: string; nickname: string; app_version: string; started_at: string; last_seen_at: string; ended_at?: string; seconds: number };
export type DashboardUsageData = {
  summary: { total_seconds: number; total_sessions: number; launches_24h: number; users_24h: number };
  users: UsageUser[]; user_count: number; page: number; page_size: number; recent_sessions: Launch[];
};
const date = (value?: string) => value ? new Date(value).toLocaleString("ru", { day: "2-digit", month: "2-digit", year: "numeric", hour: "2-digit", minute: "2-digit" }) : "Ещё не запускал";
export function usageDuration(seconds: number) {
  const minutes = Math.floor(Math.max(0, seconds) / 60);
  if (minutes < 1) return seconds > 0 ? "Меньше минуты" : "0 мин";
  return minutes < 60 ? `${minutes} мин` : `${Math.floor(minutes / 60).toLocaleString("ru")} ч ${minutes % 60} мин`;
}

export function DashboardUsage({ data, generatedAt, busy, search, onSearch, onPage }: { data: DashboardUsageData; generatedAt: string; busy: boolean; search: string; onSearch: (value: string) => void; onPage: (page: number) => void }) {
  const [input, setInput] = useState(search);
  const pages = Math.max(1, Math.ceil(data.user_count / data.page_size));
  return <div className="dashboard-usage">
    <section className="vlt-card vlt-card-pad vlt-stack" aria-labelledby="usage-users-title" aria-busy={busy}>
      <div className="usage-heading"><div><h2 className="vlt-section-title" id="usage-users-title">Время в программе</h2><p className="vlt-subtitle">Всего {data.summary.total_sessions.toLocaleString("ru")} запусков · {usageDuration(data.summary.total_seconds)}</p></div>
        <form className="usage-search" role="search" onSubmit={(event) => { event.preventDefault(); onSearch(input.trim()); }}>
          <label className="vlt-label" htmlFor="usage-search">Пользователь</label><div><input className="vlt-input" id="usage-search" type="search" placeholder="Имя или email" value={input} onChange={(event) => setInput(event.target.value)} /><button className="vlt-button vlt-button-secondary" disabled={busy}>Найти</button></div>
        </form>
      </div>
      <p className="usage-note">Время открытой программы до последнего отчёта. Это не время активного редактирования; параллельные запуски учитываются отдельно.</p>
      <div className="vlt-table-wrap usage-table"><table className="vlt-table"><thead><tr><th scope="col">Пользователь</th><th scope="col">Запуски</th><th scope="col">Всего времени</th><th scope="col">Последний запуск</th></tr></thead><tbody>
        {data.users.map((user) => <tr key={user.user_id}><td><Link className="vlt-link" href={`/users/${user.user_id}`}>{user.nickname}</Link></td><td>{user.sessions.toLocaleString("ru")}</td><td className="usage-duration">{usageDuration(user.total_seconds)}</td><td>{date(user.last_started_at)}</td></tr>)}
        {!data.users.length && <tr><td colSpan={4}>Пользователи не найдены.</td></tr>}
      </tbody></table></div>
      <div className="usage-pagination"><span className="vlt-muted">{data.user_count.toLocaleString("ru")} пользователей · страница {data.page + 1} из {pages}</span><div><button className="vlt-button vlt-button-secondary" disabled={busy || data.page === 0} onClick={() => onPage(data.page - 1)}>Назад</button><button className="vlt-button vlt-button-secondary" disabled={busy || data.page + 1 >= pages} onClick={() => onPage(data.page + 1)}>Далее</button></div></div>
    </section>
    <section className="vlt-card vlt-card-pad vlt-stack" aria-labelledby="recent-launches-title">
      <div><h2 className="vlt-section-title" id="recent-launches-title">Последние запуски</h2><p className="vlt-subtitle">20 последних сессий: кто, когда и какую версию открыл. Время указано в вашем часовом поясе.</p></div>
      <div className="vlt-table-wrap usage-table"><table className="vlt-table"><thead><tr><th scope="col">Пользователь / версия</th><th scope="col">Запуск</th><th scope="col">Последний отчёт</th><th scope="col">Время</th><th scope="col">Состояние</th></tr></thead><tbody>
        {data.recent_sessions.map((launch) => { const online = !launch.ended_at && Date.parse(generatedAt) - Date.parse(launch.last_seen_at) <= 600000; return <tr key={launch.id}><td><Link className="vlt-link" href={`/users/${launch.user_id}`}>{launch.nickname}</Link><small className="usage-version">VLTONE {launch.app_version || "—"}</small></td><td>{date(launch.started_at)}</td><td>{date(launch.ended_at ?? launch.last_seen_at)}</td><td className="usage-duration">{usageDuration(launch.seconds)}</td><td><span className={`vlt-badge${online ? " usage-online" : ""}`}>{launch.ended_at ? "Завершён" : online ? "В программе" : "Нет свежих отчётов"}</span></td></tr>; })}
        {!data.recent_sessions.length && <tr><td colSpan={5}>Запусков пока нет.</td></tr>}
      </tbody></table></div>
    </section>
  </div>;
}
