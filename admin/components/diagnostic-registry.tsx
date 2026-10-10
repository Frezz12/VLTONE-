"use client";

import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { Download, RefreshCw, Save, Search } from "lucide-react";
import { useEffect, useState } from "react";
import { AdminShell } from "./admin-shell";
import { adminPollingAllowed } from "./admin-activity";
import { canAdmin } from "./admin-permissions";
import { useAdmin } from "./use-admin";
import { downloadCsv } from "./export-csv";

type BugReport = { id: string; number: number; user_id: string; title: string; description: string; status: string; internal_note: string; created_at: string };
type CrashHealthSample = { recorded_at?: string; process_cpu?: number; system_cpu?: number; dsp_load?: number; dsp_peak?: number; xruns?: number; resident_bytes?: number; sample_rate?: number; buffer_frames?: number; track_count?: number; clip_count?: number; playback_state?: string; last_plugin?: string };
type CrashReport = { id: string; user_id: string; device_id: string; build_id: string; app_version: string; platform: string; reason: string; last_plugin?: string; artifact_bytes: number; occurred_at: string; metadata?: { signal?: string; exception_code?: string; health_samples?: CrashHealthSample[]; modules?: Array<{ name?: string; version?: string; base_address?: string }> } };
type AuditEntry = { id: string; action: string; target_type: string; target_hash: string; ip: string; created_at: string };
type Draft = { status: string; internal_note: string };

export function DiagnosticRegistry({ kind }: { kind: "bugs" | "crashes" | "audit" }) {
  const { session, error: sessionError } = useAdmin();
  const canWrite = canAdmin(session?.admin, "bugs.write");
  const [items, setItems] = useState<Array<BugReport | CrashReport | AuditEntry>>([]);
  const [drafts, setDrafts] = useState<Record<string, Draft>>({});
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const [query, setQuery] = useState("");
  const [statusFilter, setStatusFilter] = useState("");
  const [period, setPeriod] = useState("");
  const [platform, setPlatform] = useState("");
  const [saving, setSaving] = useState<string>();

  async function load(savedID?: string) {
    setLoading(true);
    try {
      const value = await api.request<{ bugs?: BugReport[]; crashes?: CrashReport[]; entries?: AuditEntry[] }>(`/v1/admin/${kind}`);
      const next = value.bugs ?? value.crashes ?? value.entries ?? [];
      setItems(next);
      setError("");
      if (value.bugs) setDrafts(previous => Object.fromEntries(value.bugs!.map(bug => [bug.id, bug.id !== savedID && previous[bug.id] ? previous[bug.id] : { status: bug.status, internal_note: bug.internal_note ?? "" }])));
    } catch (reason) {
      setError((reason as APIError).message || "Не удалось загрузить список.");
    } finally { setLoading(false); }
  }
  useEffect(() => {
    if (!session) return;
    const params = new URLSearchParams(window.location.search);
    setStatusFilter(params.get("status") ?? ""); setPeriod(params.get("period") ?? "");
    void load();
    if (kind !== "crashes") return;
    const timer = window.setInterval(() => { if (adminPollingAllowed()) void load(); }, 15000);
    return () => window.clearInterval(timer);
  }, [session, kind]);

  async function saveBug(id: string) {
    if (!session) return;
    setSaving(id);
    setError("");
    try {
      await api.json(`/v1/admin/bugs/${id}`, "PATCH", drafts[id], session.csrf_token);
      await load(id);
    } catch (reason) {
      setError((reason as APIError).message);
    } finally { setSaving(undefined); }
  }

  const title = kind === "bugs" ? "Баг-репорты" : kind === "crashes" ? "Краши" : "Аудит";
  const statuses = [{ value: "new", label: "Новый" }, { value: "triage", label: "На проверке" }, { value: "in_progress", label: "В работе" }, { value: "fixed", label: "Исправлен" }, { value: "duplicate", label: "Дубликат" }, { value: "wont_fix", label: "Без исправления" }];
  const days = period === "day" ? 1 : period === "week" ? 7 : period === "month" ? 30 : 0;
  const visibleItems = items.filter(raw => {
    const date = "occurred_at" in raw ? raw.occurred_at : raw.created_at;
    if (days && new Date(date).getTime() < Date.now() - days * 86400000) return false;
    if (kind === "bugs" && statusFilter) { const status = (raw as BugReport).status; if (statusFilter === "open" ? !["new", "triage", "in_progress"].includes(status) : status !== statusFilter) return false; }
    if (kind === "crashes" && platform && (raw as CrashReport).platform !== platform) return false;
    const searchable = kind === "bugs" ? [(raw as BugReport).number, (raw as BugReport).title, (raw as BugReport).description, (raw as BugReport).internal_note] : kind === "crashes" ? [(raw as CrashReport).reason, (raw as CrashReport).last_plugin, (raw as CrashReport).app_version, (raw as CrashReport).build_id] : [(raw as AuditEntry).action, (raw as AuditEntry).target_type, (raw as AuditEntry).target_hash, (raw as AuditEntry).ip];
    return searchable.join(" ").toLowerCase().includes(query.trim().toLowerCase());
  });
  function exportItems() {
    if (kind === "bugs") return downloadCsv("vltone-bugs.csv", ["Номер", "Заголовок", "Описание", "Статус", "Заметка", "Дата"], visibleItems.map(raw => { const item = raw as BugReport; return [item.number, item.title, item.description, item.status, item.internal_note, item.created_at]; }));
    if (kind === "crashes") return downloadCsv("vltone-crashes.csv", ["Причина", "Плагин", "Версия", "Сборка", "Платформа", "Дата"], visibleItems.map(raw => { const item = raw as CrashReport; return [item.reason, item.last_plugin, item.app_version, item.build_id, item.platform, item.occurred_at]; }));
    downloadCsv("vltone-audit.csv", ["Действие", "Тип", "Цель", "IP", "Дата"], visibleItems.map(raw => { const item = raw as AuditEntry; return [item.action, item.target_type, item.target_hash, item.ip, item.created_at]; }));
  }
  return <AdminShell>
    <div className="admin-page-head"><div><h1 className="vlt-title">{title}</h1><p className="vlt-subtitle">{kind === "audit" ? "История действий: кто и что изменил в панели управления." : kind === "bugs" ? "Обратная связь, приоритет работы и внутренние заметки команды." : "Причины сбоев, версии приложения и логи для расследования."}</p></div><div className="admin-page-actions"><button className="vlt-button vlt-button-secondary" onClick={() => void load()} disabled={loading || !!saving}><RefreshCw size={16} aria-hidden />Обновить</button><button className="vlt-button vlt-button-secondary" onClick={exportItems} disabled={loading || !visibleItems.length}><Download size={16} aria-hidden />Выгрузить CSV</button></div></div>
    {(sessionError || error) && <div className="vlt-error" style={{ marginBottom: 16 }}>{sessionError || error}</div>}
    <div className="admin-list-toolbar"><label className="diagnostic-search"><Search size={17} aria-hidden /><input type="search" className="vlt-input" aria-label="Поиск в списке" placeholder={kind === "bugs" ? "Заголовок, номер или заметка" : kind === "crashes" ? "Причина, плагин или версия" : "Действие, цель или IP"} value={query} onChange={event => setQuery(event.target.value)} /></label>{kind === "bugs" && <label className="admin-inline-filter">Статус<select className="vlt-input" aria-label="Статус" value={statusFilter} onChange={event => setStatusFilter(event.target.value)}><option value="">Все статусы</option><option value="open">Открытые</option>{statuses.map(item => <option key={item.value} value={item.value}>{item.label}</option>)}</select></label>}{kind === "crashes" && <label className="admin-inline-filter">Платформа<select className="vlt-input" aria-label="Платформа" value={platform} onChange={event => setPlatform(event.target.value)}><option value="">Все платформы</option>{Array.from(new Set(items.map(item => (item as CrashReport).platform))).sort().map(value => <option key={value} value={value}>{value}</option>)}</select></label>}<label className="admin-inline-filter">Период<select className="vlt-input" aria-label="Период" value={period} onChange={event => setPeriod(event.target.value)}><option value="">За всё время</option><option value="day">24 часа</option><option value="week">7 дней</option><option value="month">30 дней</option></select></label><span className="admin-result-count" role="status">{loading ? "Загрузка…" : `В списке: ${visibleItems.length} из ${items.length}`}</span></div>
    <div className="vlt-table-wrap"><table className="vlt-table diagnostic-table"><thead><tr>
      {kind === "bugs" ? <><th>№ / заголовок</th><th>Статус</th><th>Внутренняя заметка</th><th>Создан</th><th /></> : kind === "crashes" ? <><th>Причина</th><th>Версия / build</th><th>Платформа</th><th>Дата</th><th /></> : <><th>Действие</th><th>Тип</th><th>Обезличенная цель</th><th>IP / дата</th></>}
    </tr></thead><tbody>{visibleItems.map((raw) => {
      if (kind === "bugs") {
        const item = raw as BugReport; const draft = drafts[item.id] ?? { status: item.status, internal_note: item.internal_note ?? "" };
        return <tr key={item.id}><td><span className="vlt-code">#{item.number}</span><div><strong>{item.title}</strong></div><div className="vlt-muted registry-description">{item.description}</div></td><td><label className="sr-only" htmlFor={`status-${item.id}`}>Статус бага #{item.number}</label><select disabled={!canWrite} id={`status-${item.id}`} className="vlt-input" value={draft.status} onChange={(event) => setDrafts((old) => ({ ...old, [item.id]: { ...draft, status: event.target.value } }))}>{statuses.map(status => <option key={status.value} value={status.value}>{status.label}</option>)}</select></td><td><label className="sr-only" htmlFor={`note-${item.id}`}>Внутренняя заметка</label><textarea disabled={!canWrite} id={`note-${item.id}`} className="vlt-input registry-note" value={draft.internal_note} onChange={(event) => setDrafts((old) => ({ ...old, [item.id]: { ...draft, internal_note: event.target.value } }))} /></td><td>{new Date(item.created_at).toLocaleString("ru")}</td><td><button className="vlt-button vlt-button-secondary" disabled={!canWrite || !!saving} onClick={() => void saveBug(item.id)}><Save size={15} aria-hidden />{saving === item.id ? "Сохранение…" : "Сохранить"}</button></td></tr>;
      }
      if (kind === "crashes") {
        const item = raw as CrashReport;
        const health = item.metadata?.health_samples ?? [];
        const latest = health.at(-1);
        const signal = item.metadata?.signal || item.metadata?.exception_code;
        return <tr key={item.id}><td><strong>{item.reason}</strong>{item.last_plugin && <div className="vlt-muted">Плагин: {item.last_plugin}</div>}{signal && <div className="vlt-code vlt-muted">{signal}</div>}{latest && <details className="crash-context"><summary>Контекст перед крашем · {health.length} samples</summary><div className="vlt-muted">CPU {latest.process_cpu?.toFixed(1) ?? "—"}% / {latest.system_cpu?.toFixed(1) ?? "—"}% · DSP {latest.dsp_load?.toFixed(1) ?? "—"}% (peak {latest.dsp_peak?.toFixed(1) ?? "—"}%)</div><div className="vlt-muted">RAM {latest.resident_bytes ? `${(latest.resident_bytes / 1048576).toFixed(0)} MB` : "—"} · tracks/clips {latest.track_count ?? "—"}/{latest.clip_count ?? "—"} · buffer {latest.buffer_frames ?? "—"} @ {latest.sample_rate ?? "—"} Hz</div>{item.metadata?.modules?.length ? <div className="vlt-muted">Modules: {item.metadata.modules.map((module) => module.name).filter(Boolean).join(", ")}</div> : null}</details>}</td><td><span className="vlt-code">{item.app_version}</span><div className="vlt-muted vlt-code">{item.build_id}</div></td><td>{item.platform}</td><td>{new Date(item.occurred_at).toLocaleString("ru")}</td><td>{item.artifact_bytes > 0 && <a className="vlt-link vlt-row" href={`/api/v1/admin/crashes/${item.id}/artifact`}><Download size={15} /> Скачать логи</a>}</td></tr>;
      }
      const item = raw as AuditEntry;
      return <tr key={item.id}><td>{item.action}</td><td>{item.target_type}</td><td className="vlt-code">{item.target_hash.slice(0, 18)}…</td><td>{item.ip}<div className="vlt-muted">{new Date(item.created_at).toLocaleString("ru")}</div></td></tr>;
    })}{!loading && !visibleItems.length && <tr><td colSpan={kind === "audit" ? 4 : 5} className="admin-empty">Ничего не найдено. Измените поиск или фильтры.</td></tr>}</tbody></table></div>
  </AdminShell>;
}
