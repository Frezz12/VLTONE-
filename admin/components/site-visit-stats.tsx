"use client";

import Link from "next/link";
import { useEffect, useState } from "react";
import { api } from "@vlt/api-client";
import { Bar, BarChart, CartesianGrid, Legend, ResponsiveContainer, Tooltip, XAxis, YAxis } from "recharts";
import { AdminShell } from "./admin-shell";
import { canAdmin } from "./admin-permissions";
import { useAdmin } from "./use-admin";

type Counts = { views: number; visitors: number; accounts: number };
type Stats = {
  today: Counts;
  daily: (Counts & { day: string })[];
  pages: { path: string; views: number }[];
  visits: { id: string; visited_at: string; visitor_key: string; user_id: string | null; nickname: string; path: string; referrer_host: string; browser: string; os: string }[];
  has_more: boolean;
  generated_at: string;
};
const number = new Intl.NumberFormat("ru-RU");
const date = new Intl.DateTimeFormat("ru-RU", { timeZone: "Europe/Moscow", day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit" });

export function SiteVisitStats() {
  const { session, error: sessionError } = useAdmin();
  const [days, setDays] = useState(7);
  const [page, setPage] = useState(0);
  const [refresh, setRefresh] = useState(0);
  const [data, setData] = useState<Stats>();
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState("");
  const allowed = canAdmin(session?.admin, "analytics.read");
  useEffect(() => {
    if (!allowed) return;
    const controller = new AbortController();
    setBusy(true); setError(""); setData(undefined);
    api.request<Stats>(`/v1/admin/analytics?days=${days}&page=${page}`, { signal: controller.signal })
      .then(result => { if (!controller.signal.aborted) setData(result); })
      .catch(() => { if (!controller.signal.aborted) setError("Не удалось загрузить посещения. Нажмите «Обновить», чтобы повторить."); })
      .finally(() => { if (!controller.signal.aborted) setBusy(false); });
    return () => controller.abort();
  }, [allowed, days, page, refresh]);

  return <AdminShell>
    <div className="vlt-page-header"><div><h1 className="vlt-title">Посещения сайта</h1><p className="vlt-subtitle">Кто открывает сайт, какие страницы смотрит и откуда приходит. Время — московское (UTC+3).</p></div></div>
    {sessionError && <p className="vlt-error" role="alert">{sessionError}</p>}
    {session && !allowed ? <p className="vlt-error" role="alert">Нет доступа к статистике посещений.</p> : <>
      <div className="vlt-row vlt-between site-stats-toolbar">
        <label className="vlt-row" htmlFor="site-stats-days">Период <select className="vlt-input" id="site-stats-days" value={days} onChange={event => { setDays(Number(event.target.value)); setPage(0); }}><option value={1}>Сегодня</option><option value={7}>7 дней</option><option value={30}>30 дней</option></select></label>
        <button type="button" className="vlt-button vlt-button-secondary" disabled={busy || !allowed} onClick={() => setRefresh(value => value + 1)}>{busy ? "Загрузка…" : "Обновить"}</button>
      </div>
      {error && <p className="vlt-error" role="alert">{error}</p>}
      {busy && !error && <p role="status" className="vlt-muted">Загружаем статистику посещений…</p>}
      {data && <div className="site-stats-content">
        <div className="vlt-grid vlt-grid-4 dashboard-metrics">
          {[["Просмотры сегодня", data.today.views], ["Посетители сегодня ≈", data.today.visitors], ["Аккаунты сегодня", data.today.accounts], ["Просмотры за период", data.daily.reduce((sum, item) => sum + item.views, 0)]].map(([label, value], index) => <section className="vlt-card vlt-stat" data-metric={index} key={label}><span className="vlt-stat-label">{label}</span><div className="vlt-stat-value">{number.format(Number(value))}</div></section>)}
        </div>
        <p className="vlt-muted">Каждое открытие и переход на страницу — один просмотр. Посетители — оценка по браузеру и сети за день: общий интернет может объединять людей, смена сети — разделять. Аккаунты считаются отдельно. Данные появляются с момента включения; блокировщики и запрет отслеживания могут уменьшать числа.</p>
        <section className="vlt-card vlt-card-pad" aria-labelledby="site-daily-title">
          <h2 id="site-daily-title" className="vlt-section-title">Посещения по дням</h2>
          <div className="site-stats-chart" aria-hidden="true"><ResponsiveContainer width="100%" height="100%"><BarChart data={data.daily} maxBarSize={48}><CartesianGrid vertical={false} stroke="var(--vlt-border)" /><XAxis dataKey="day" tickFormatter={value => String(value).slice(5).split("-").reverse().join(".")} tick={{ fill: "var(--vlt-text-muted)", fontSize: 12 }} /><YAxis allowDecimals={false} tick={{ fill: "var(--vlt-text-muted)", fontSize: 12 }} /><Tooltip contentStyle={{ background: "var(--vlt-surface-raised)", border: "1px solid var(--vlt-border)", borderRadius: 8 }} /><Legend /><Bar dataKey="views" name="Просмотры" fill="var(--vlt-accent)" isAnimationActive={false} /><Bar dataKey="visitors" name="Посетители ≈" fill="var(--vlt-text-muted)" isAnimationActive={false} /></BarChart></ResponsiveContainer></div>
          <details><summary>Числа по дням</summary><div className="site-stats-table"><table className="vlt-table"><caption className="sr-only">Посещения по московским календарным дням</caption><thead><tr><th scope="col">Дата</th><th scope="col">Просмотры</th><th scope="col">Посетители ≈</th><th scope="col">Аккаунты</th></tr></thead><tbody>{data.daily.map(row => <tr key={row.day}><th scope="row">{row.day}</th><td>{number.format(row.views)}</td><td>{number.format(row.visitors)}</td><td>{number.format(row.accounts)}</td></tr>)}</tbody></table></div></details>
        </section>
        <section className="vlt-card vlt-card-pad" aria-labelledby="site-pages-title"><h2 id="site-pages-title" className="vlt-section-title">Популярные страницы</h2>{data.pages.length === 0 ? <p className="vlt-muted">За этот период просмотров пока нет.</p> : <div className="site-stats-table"><table className="vlt-table"><caption className="sr-only">20 самых посещаемых страниц за выбранный период</caption><thead><tr><th scope="col">Страница</th><th scope="col">Просмотры</th></tr></thead><tbody>{data.pages.map(row => <tr key={row.path}><th scope="row" className="site-stats-path">{row.path}</th><td>{number.format(row.views)}</td></tr>)}</tbody></table></div>}</section>
        <section className="vlt-card vlt-card-pad" aria-labelledby="site-visits-title">
          <h2 id="site-visits-title" className="vlt-section-title">Журнал посещений</h2>
          <p className="vlt-subtitle">Имена видны для вошедших пользователей. Анонимный номер действует только в пределах одного дня. IP-адреса и параметры ссылок не сохраняются.</p>
          {data.visits.length === 0 ? <p className="vlt-muted">Посещений пока нет.</p> : <div className="site-stats-table" role="region" aria-label="Журнал посещений, прокручиваемая таблица" tabIndex={0}><table className="vlt-table"><thead><tr><th scope="col">Время (МСК)</th><th scope="col">Посетитель</th><th scope="col">Страница</th><th scope="col">Источник</th><th scope="col">Браузер / ОС</th></tr></thead><tbody>{data.visits.map(row => <tr key={row.id}><td><time dateTime={row.visited_at}>{date.format(new Date(row.visited_at))}</time></td><td>{row.user_id ? (canAdmin(session?.admin, "users.read") ? <Link href={`/users/${row.user_id}`}>{row.nickname || "Пользователь"}</Link> : row.nickname || "Пользователь") : <span>Гость {row.visitor_key.slice(0, 8)}</span>}</td><td className="site-stats-path">{row.path}</td><td>{row.referrer_host || "Прямой / не указан"}</td><td>{row.browser} / {row.os}</td></tr>)}</tbody></table></div>}
          <div className="vlt-row vlt-between site-stats-pagination"><button type="button" className="vlt-button vlt-button-secondary" disabled={page === 0} onClick={() => setPage(value => value - 1)}>Назад</button><span aria-live="polite">Страница {page + 1}</span><button type="button" className="vlt-button vlt-button-secondary" disabled={!data.has_more || page >= 10000} onClick={() => setPage(value => value + 1)}>Далее</button></div>
        </section>
        <p className="vlt-muted">Обновлено {date.format(new Date(data.generated_at))} МСК</p>
      </div>}
    </>}
  </AdminShell>;
}
