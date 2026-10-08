"use client";

import { api } from "@vlt/api-client";
import { Activity, ArrowUpRight, Bug, CheckCircle2, Clock3, Cpu, Play, RefreshCw, Users } from "lucide-react";
import Link from "next/link";
import { useEffect, useState } from "react";
import { Area, AreaChart, Bar, BarChart, CartesianGrid, ResponsiveContainer, Tooltip, XAxis, YAxis } from "recharts";
import { AdminShell } from "./admin-shell";
import { adminPollingAllowed } from "./admin-activity";
import { useAdmin } from "./use-admin";
import { DashboardUsage, type DashboardUsageData } from "./dashboard-usage";

type ActivityPoint = { bucket: string; sessions: number; crashes: number };
type AIPoint = { bucket: string; tokens: number };
type OnlineUser = { user_id: string; nickname: string; last_seen_at: string; sessions: number };
type DashboardData = { users: number; active_sessions: number; crashes_24h: number; open_bugs: number; ai_tokens_month: number; generated_at: string; activity?: ActivityPoint[]; ai_daily?: AIPoint[]; online_users?: OnlineUser[]; usage?: DashboardUsageData };
const chartText = { fill: "var(--vlt-text-muted)", fontSize: 11 };
const tooltipStyle = { color: "var(--vlt-text)", background: "var(--vlt-surface-raised)", border: "1px solid var(--vlt-border)", borderRadius: 9 };

export function Dashboard() {
  const { session, error } = useAdmin();
  const [data, setData] = useState<DashboardData>();
  const [usageSearch, setUsageSearch] = useState("");
  const [usagePage, setUsagePage] = useState(0);
  const [loading, setLoading] = useState(true);
  const [loadError, setLoadError] = useState("");
  const [refresh, setRefresh] = useState(0);
  useEffect(() => {
    if (!session) return;
    const controller = new AbortController();
    let pending = false;
    async function load() {
      if (pending) return;
      pending = true;
      setLoading(true);
      try {
        const query = new URLSearchParams({ usage_q: usageSearch, usage_page: String(usagePage) });
        const result = await api.request<DashboardData>(`/v1/admin/dashboard?${query}`, { signal: controller.signal });
        if (!controller.signal.aborted) { setData(result); setLoadError(""); }
      } catch {
        if (!controller.signal.aborted) setLoadError("Не удалось обновить статистику. Повторите попытку кнопкой «Обновить».");
      } finally {
        pending = false;
        if (!controller.signal.aborted) setLoading(false);
      }
    }
    void load();
    const timer = window.setInterval(() => { if (adminPollingAllowed()) void load(); }, 15000);
    return () => { controller.abort(); window.clearInterval(timer); };
  }, [session, usageSearch, usagePage, refresh]);
  const cards = data ? [["Пользователи", data.users, Users], ["Активные сессии", data.active_sessions, Activity], ["Всего часов в программе", (data.usage?.summary.total_seconds ?? 0) / 3600, Clock3], ["Запуски / 24 часа", data.usage?.summary.launches_24h ?? 0, Play], ["Пользователи / 24 часа", data.usage?.summary.users_24h ?? 0, Users], ["Краши / 24 часа", data.crashes_24h, Bug], ["AI-токены / месяц", data.ai_tokens_month, Cpu], ["Открытые баги", data.open_bugs, Bug]] as const : [];
  const activity = (data?.activity ?? []).map((point) => ({ ...point, label: new Date(point.bucket).toLocaleTimeString("ru", { hour: "2-digit", minute: "2-digit" }) }));
  const ai = (data?.ai_daily ?? []).map((point) => ({ ...point, label: new Date(point.bucket).toLocaleDateString("ru", { day: "2-digit", month: "2-digit" }) }));
  return <AdminShell>
    <div className="admin-page-head"><div><h1 className="vlt-title">Оперативный обзор</h1><p className="vlt-subtitle">Сервис, пользователи и диагностика.</p></div><div className="dashboard-update"><button className="vlt-button vlt-button-secondary" disabled={loading || !session} onClick={() => setRefresh(value => value + 1)}><RefreshCw size={16} aria-hidden />Обновить</button>{data && <span className="vlt-muted">Обновлено {new Date(data.generated_at).toLocaleTimeString("ru", { hour: "2-digit", minute: "2-digit" })}</span>}</div></div>
    {error && <div className="vlt-error">{error}</div>}
    {loadError && <div className="vlt-error" role="status">{loadError}</div>}
    {!data && !error && !loadError && <p className="vlt-muted" role="status">Загружаем статистику…</p>}
    {data && <section className="dashboard-priority" aria-label="Требует внимания"><div><span className="admin-kicker">Сегодня в работе</span><h2>{data.crashes_24h || data.open_bugs ? "Требует внимания" : "Диагностика в порядке"}</h2><p>{data.crashes_24h || data.open_bugs ? "Откройте репорты, чтобы проверить причины и обновить статусы." : "Открытых багов и крашей за последние сутки нет."}</p></div><div className="dashboard-priority-links">{data.open_bugs > 0 && <Link href="/bugs?status=open"><Bug size={18} aria-hidden /><span>Открытых багов<strong>{data.open_bugs}</strong></span><ArrowUpRight size={18} aria-hidden /></Link>}{data.crashes_24h > 0 && <Link href="/crashes?period=day"><Activity size={18} aria-hidden /><span>Краши за 24 часа<strong>{data.crashes_24h}</strong></span><ArrowUpRight size={18} aria-hidden /></Link>}{!data.open_bugs && !data.crashes_24h && <CheckCircle2 size={36} aria-hidden />}</div></section>}
    <div className="vlt-grid vlt-grid-4 dashboard-metrics">{cards.map(([label, value, Icon], index) => <section className="vlt-card vlt-stat" data-metric={index} key={label}><div className="vlt-row vlt-between"><span className="vlt-stat-label">{label}</span><Icon size={19} className="vlt-muted" aria-hidden /></div><div className="vlt-stat-value">{new Intl.NumberFormat("ru", { maximumFractionDigits: 1 }).format(value)}</div></section>)}</div>
    {data && <>
      <div className="vlt-grid vlt-grid-2 dashboard-charts">
        <section className="vlt-card vlt-card-pad vlt-stack" aria-label="График запусков и крашей за 24 часа"><h2 className="vlt-section-title">Запуски и краши · 24 часа</h2><div className="chart-frame"><ResponsiveContainer width="100%" height="100%"><AreaChart data={activity}><defs><linearGradient id="activityFill" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stopColor="var(--vlt-accent)" stopOpacity={0.28} /><stop offset="100%" stopColor="var(--vlt-accent)" stopOpacity={0.01} /></linearGradient></defs><CartesianGrid stroke="var(--vlt-border-muted)" vertical={false} /><XAxis dataKey="label" tick={chartText} minTickGap={24} /><YAxis tick={chartText} allowDecimals={false} width={30} /><Tooltip contentStyle={tooltipStyle} /><Area isAnimationActive={false} type="monotone" dataKey="sessions" name="Запуски" stroke="var(--vlt-accent)" strokeWidth={2} fill="url(#activityFill)" /><Area isAnimationActive={false} type="monotone" dataKey="crashes" name="Краши" stroke="var(--vlt-danger)" strokeWidth={2} fill="transparent" /></AreaChart></ResponsiveContainer></div></section>
        <section className="vlt-card vlt-card-pad vlt-stack" aria-label="График расхода AI токенов за месяц"><h2 className="vlt-section-title">AI-расход · текущий UTC-месяц</h2><div className="chart-frame"><ResponsiveContainer width="100%" height="100%"><BarChart data={ai}><CartesianGrid stroke="var(--vlt-border-muted)" vertical={false} /><XAxis dataKey="label" tick={chartText} minTickGap={20} /><YAxis tick={chartText} width={52} tickFormatter={(value) => new Intl.NumberFormat("ru", { notation: "compact" }).format(Number(value))} /><Tooltip contentStyle={tooltipStyle} formatter={(value) => new Intl.NumberFormat("ru").format(Number(value))} /><Bar isAnimationActive={false} dataKey="tokens" name="Токены" fill="var(--vlt-accent)" radius={[4, 4, 0, 0]} /></BarChart></ResponsiveContainer></div></section>
      </div>
      <section className="vlt-card vlt-card-pad vlt-stack online-users" aria-labelledby="online-users-title">
        <div className="vlt-row vlt-between"><div><h2 className="vlt-section-title" id="online-users-title">Сейчас онлайн</h2><p className="vlt-subtitle">Пользователи с активностью за последние 10 минут.</p></div><span className="vlt-badge"><span className="status-dot" />{data.online_users?.length ?? 0}</span></div>
        {data.online_users?.length ? <ul>{data.online_users.map((user) => <li key={user.user_id}><span className="status-dot" aria-hidden /><span><Link className="vlt-link" href={`/users/${user.user_id}`}>{user.nickname}</Link><small>Активных сессий: {user.sessions} · обновлён {new Date(user.last_seen_at).toLocaleTimeString("ru", { hour: "2-digit", minute: "2-digit" })}</small></span></li>)}</ul> : <p className="vlt-muted">Сейчас никого нет онлайн.</p>}
      </section>
      {data.usage && <DashboardUsage data={data.usage} generatedAt={data.generated_at} busy={loading} search={usageSearch} onSearch={(value) => { setUsageSearch(value); setUsagePage(0); }} onPage={setUsagePage} />}
    </>}
  </AdminShell>;
}
