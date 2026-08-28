"use client";

import { api } from "@vlt/api-client";
import { Activity, Bug, Cpu, Users } from "lucide-react";
import { useEffect, useState } from "react";
import { Area, AreaChart, Bar, BarChart, CartesianGrid, ResponsiveContainer, Tooltip, XAxis, YAxis } from "recharts";
import { AdminShell } from "./admin-shell";
import { adminPollingAllowed } from "./admin-activity";
import { useAdmin } from "./use-admin";

type ActivityPoint = { bucket: string; sessions: number; crashes: number };
type AIPoint = { bucket: string; tokens: number };
type DashboardData = { users: number; active_sessions: number; crashes_24h: number; open_bugs: number; ai_tokens_month: number; generated_at: string; activity?: ActivityPoint[]; ai_daily?: AIPoint[] };
const chartText = { fill: "#62665f", fontSize: 11 };
const tooltipStyle = { color: "#181a17", background: "#ffffff", border: "1px solid #dcded8", borderRadius: 7 };

export function Dashboard() {
  const { session, error } = useAdmin();
  const [data, setData] = useState<DashboardData>();
  useEffect(() => {
    if (!session) return;
    const load = () => api.request<DashboardData>("/v1/admin/dashboard").then(setData);
    void load();
    const timer = window.setInterval(() => { if (adminPollingAllowed()) void load(); }, 15000);
    return () => window.clearInterval(timer);
  }, [session]);
  const cards = data ? [["Пользователи", data.users, Users], ["Активные сессии", data.active_sessions, Activity], ["AI-токены / месяц", data.ai_tokens_month, Cpu], ["Открытые баги", data.open_bugs, Bug]] as const : [];
  const activity = (data?.activity ?? []).map((point) => ({ ...point, label: new Date(point.bucket).toLocaleTimeString("ru", { hour: "2-digit", minute: "2-digit" }) }));
  const ai = (data?.ai_daily ?? []).map((point) => ({ ...point, label: new Date(point.bucket).toLocaleDateString("ru", { day: "2-digit", month: "2-digit" }) }));
  return <AdminShell>
    <div className="admin-page-head"><div><h1 className="vlt-title">Оперативный обзор</h1><p className="vlt-subtitle">Активность VLT Studio Pro и диагностика.</p></div>{data && <span className="vlt-badge"><span className="status-dot" />API обновлён {new Date(data.generated_at).toLocaleTimeString("ru")}</span>}</div>
    {error && <div className="vlt-error">{error}</div>}
    <div className="vlt-grid vlt-grid-4">{cards.map(([label, value, Icon]) => <section className="vlt-card vlt-stat" key={label}><div className="vlt-row vlt-between"><span className="vlt-stat-label">{label}</span><Icon size={16} className="vlt-muted" aria-hidden /></div><div className="vlt-stat-value">{new Intl.NumberFormat("ru").format(value)}</div></section>)}</div>
    {data && <>
      <div className="vlt-grid vlt-grid-2 dashboard-charts">
        <section className="vlt-card vlt-card-pad vlt-stack" aria-label="График запусков и крашей за 24 часа"><h2 className="vlt-section-title">Запуски и краши · 24 часа</h2><div className="chart-frame"><ResponsiveContainer width="100%" height="100%"><AreaChart data={activity}><defs><linearGradient id="activityFill" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stopColor="#2446d8" stopOpacity={0.22} /><stop offset="100%" stopColor="#2446d8" stopOpacity={0.01} /></linearGradient></defs><CartesianGrid stroke="#e5e6e1" vertical={false} /><XAxis dataKey="label" tick={chartText} minTickGap={24} /><YAxis tick={chartText} allowDecimals={false} width={30} /><Tooltip contentStyle={tooltipStyle} /><Area type="monotone" dataKey="sessions" name="Запуски" stroke="#2446d8" strokeWidth={2} fill="url(#activityFill)" /><Area type="monotone" dataKey="crashes" name="Краши" stroke="#be3049" strokeWidth={2} fill="transparent" /></AreaChart></ResponsiveContainer></div></section>
        <section className="vlt-card vlt-card-pad vlt-stack" aria-label="График расхода AI токенов за месяц"><h2 className="vlt-section-title">AI-расход · текущий UTC-месяц</h2><div className="chart-frame"><ResponsiveContainer width="100%" height="100%"><BarChart data={ai}><CartesianGrid stroke="#e5e6e1" vertical={false} /><XAxis dataKey="label" tick={chartText} minTickGap={20} /><YAxis tick={chartText} width={52} tickFormatter={(value) => new Intl.NumberFormat("ru", { notation: "compact" }).format(Number(value))} /><Tooltip contentStyle={tooltipStyle} formatter={(value) => new Intl.NumberFormat("ru").format(Number(value))} /><Bar dataKey="tokens" name="Токены" fill="#2446d8" radius={[4, 4, 0, 0]} /></BarChart></ResponsiveContainer></div></section>
      </div>
      <section className="vlt-card vlt-card-pad vlt-stack" style={{ marginTop: 16 }}><h2 className="vlt-section-title">Состояние тестирования</h2><div className="vlt-grid vlt-grid-2"><div><span className="vlt-muted">Крашей за 24 часа</span><div className="vlt-stat-value">{data.crashes_24h}</div></div><div><span className="vlt-muted">Глобальный AI kill switch</span><div style={{ marginTop: 8 }}><span className="vlt-badge vlt-badge-accent">Управляется сервером</span></div></div></div></section>
    </>}
  </AdminShell>;
}
