"use client";

import { api } from "@vlt/api-client";
import { useEffect, useState } from "react";
import { adminPollingAllowed } from "./admin-activity";

type Session = { id: string; app_version: string; build_id: string; started_at: string; ended_at?: string; end_reason?: string; hardware: Record<string, unknown> };
type Slot = { id: string; slot: number; name: string; vendor: string; version: string; format: string; bypassed: boolean; mix: number; channel_mode: string; sidechain_track_id: string };
type ClipFX = { id: string; name: string; kind: string; start_seconds: number; duration_seconds: number; muted: boolean; inserts: Slot[]; offline_inserts: Slot[] };
type Track = { clip_fx?: ClipFX[]; id: string; name: string; kind: string; parent_id: string; output_bus_id: string; volume: number; pan: number; muted: boolean; soloed: boolean; armed: boolean; monitor: boolean; mono: boolean; frozen: boolean; input_enabled: boolean; input_channel: number; input_channel_count: number; clip_count: number; instrument?: Slot; inserts: Slot[]; sampler_fx: Slot[]; sampler_fx_active: boolean; sends: { destination_track_id: string; level: number; pre_fader: boolean; enabled: boolean }[] };
type Device = { name: string; manufacturer: string; host_api: string; input_channels: number; output_channels: number; is_asio: boolean; is_alive: boolean };
type Snapshot = { schema_version: number; truncated: boolean; window_ms: number; measurement_count: number; tempo: number; time_signature_numerator: number; time_signature_denominator: number; position_seconds: number; loop_enabled: boolean; loop_start_seconds: number; loop_end_seconds: number; master_volume: number; master_pan: number; master_inserts: Slot[]; tracks: Track[]; audio: { input: Device; output: Device; running: boolean; input_enabled: boolean; input_channels: number[]; output_channels: number[]; input_underflow: number; input_overflow: number; output_underflow: number; output_overflow: number; gated_blocks: number; workers: number; realtime_workers: number; workgroup_workers: number } };
type Sample = { id: string; event_id: string; session_id: string; device_id: string; recorded_at: string; process_cpu: number; system_cpu: number; dsp_load: number; dsp_peak: number; xruns: number; resident_bytes: number; sample_rate: number; buffer_frames: number; track_count: number; clip_count: number; plugin_count: number; playback_state: string; recording: boolean; foreground: boolean; plugins?: { name: string; vendor: string; version: string; format: string; count: number }[]; snapshot?: Snapshot };
type TelemetryPage = { sessions: Session[]; samples: Sample[]; next_cursor?: string };
type Report = { payload: Sample; session: Session; recorded_at: string; event_id: string; device_id: string };
const date = (value: string) => new Date(value).toLocaleString("ru");
const num = (value?: number, digits = 1) => typeof value === "number" && Number.isFinite(value) ? value.toLocaleString("ru", { maximumFractionDigits: digits }) : "—";
const state = (sample: Sample) => sample.recording ? "Запись" : sample.playback_state === "playing" ? "Воспроизведение" : sample.playback_state === "paused" ? "Пауза" : "Остановлено";

function Facts({ values }: { values: [string, React.ReactNode][] }) {
  return <dl className="telemetry-facts">{values.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl>;
}
function PluginChain({ title, slots, trackName }: { title: string; slots?: Slot[]; trackName: (id: string) => string }) {
  return <section className="telemetry-chain"><h4>{title} <span className="vlt-muted">· {slots?.length ?? 0}</span></h4>{slots?.length ? <ol>{slots.map((slot, index) => <li key={`${slot.id}-${index}`}>
    <div><strong>{slot.slot + 1}. {slot.name || "Без названия"}</strong><span className="vlt-muted">{[slot.format, slot.vendor, slot.version && `v${slot.version}`].filter(Boolean).join(" · ")}</span></div>
    <div className="telemetry-slot-state"><span className="vlt-badge">{slot.bypassed ? "Bypass" : "Включён"}</span><span>Mix {num(slot.mix * 100)}% · {slot.channel_mode}</span>{slot.sidechain_track_id && <span>Sidechain: {trackName(slot.sidechain_track_id)}</span>}</div>
  </li>)}</ol> : <p className="vlt-muted">Нет плагинов</p>}</section>;
}
function TrackDisclosure({ summary, children }: { summary: React.ReactNode; children: React.ReactNode }) {
  const [open, setOpen] = useState(false);
  return <details className="telemetry-track" onToggle={(event) => setOpen(event.currentTarget.open)}><summary>{summary}</summary>{open && children}</details>;
}
function ReportView({ report }: { report: Report }) {
  const [query, setQuery] = useState("");
  const sample = report.payload, snapshot = sample.snapshot;
  const tracks = snapshot?.tracks ?? [];
  const names = new Map(tracks.map((track) => [track.id, track.name]));
  const trackName = (id: string) => id ? names.get(id) || id : "Master";
  const visible = tracks.filter((track) => [track.name, track.kind, track.instrument?.name, ...(track.inserts ?? []).map((slot) => slot.name), ...(track.sampler_fx ?? []).map((slot) => slot.name), ...(track.clip_fx ?? []).flatMap((clip) => [clip.name, ...(clip.inserts ?? []).map((slot) => slot.name), ...(clip.offline_inserts ?? []).map((slot) => slot.name)])].join(" ").toLocaleLowerCase().includes(query.toLocaleLowerCase()));
  return <div className="telemetry-report vlt-stack">
    <Facts values={[["Снимок", date(report.recorded_at)], ["Версия / сборка", `${report.session.app_version || "—"} / ${report.session.build_id || "—"}`], ["Сессия", report.session.id || "—"], ["Устройство", report.device_id], ["CPU процесса / системы, среднее", `${num(sample.process_cpu)}% / ${num(sample.system_cpu)}%`], ["DSP среднее / пик измерений", `${num(sample.dsp_load)}% / ${num(sample.dsp_peak)}%`], ["Память процесса", `${num(sample.resident_bytes / 1048576)} MB`], ["Состояние", `${state(sample)} · ${sample.foreground ? "окно активно" : "в фоне"}`], ["Дорожки / клипы / плагины", `${sample.track_count} / ${sample.clip_count} / ${sample.plugin_count}`], ["Аудиосбои (накопительно)", num(sample.xruns, 0)]]} />
    {snapshot ? <>
      {snapshot.truncated && <p className="vlt-error" role="status">Снимок превышает лимит размера: часть дорожек или цепочек не включена. Счётчики выше относятся ко всему проекту.</p>}
      <Facts values={[["Окно измерений", `${num(snapshot.window_ms / 1000)} с · ${snapshot.measurement_count} измерений`], ["Темп / размер", `${num(snapshot.tempo)} BPM · ${snapshot.time_signature_numerator}/${snapshot.time_signature_denominator}`], ["Позиция", `${num(snapshot.position_seconds)} с`], ["Цикл", snapshot.loop_enabled ? `${num(snapshot.loop_start_seconds)}–${num(snapshot.loop_end_seconds)} с` : "Выключен"], ["Master: громкость / панорама", `${num(snapshot.master_volume)} / ${num(snapshot.master_pan)}`]]} />
      <section><h3 className="vlt-section-title">Аудиоустройства и движок</h3><div className="telemetry-devices">{([['Вход', snapshot.audio.input], ['Выход', snapshot.audio.output]] as [string, Device][]).map(([label, device]) => <div className="telemetry-device" key={label}><h4>{label}</h4><strong>{device.name || "Не подключено"}</strong><p className="vlt-muted">{[device.manufacturer, device.host_api].filter(Boolean).join(" · ") || "Драйвер не указан"}</p><p>{device.input_channels} входов · {device.output_channels} выходов{device.name && !device.is_alive ? " · Недоступно" : ""}</p></div>)}</div>
        <Facts values={[["Аудиопоток", snapshot.audio.running ? "Работает" : "Остановлен"], ["Частота / буфер", `${num(sample.sample_rate, 0)} Hz / ${sample.buffer_frames} frames`], ["Длительность буфера", sample.sample_rate > 0 ? `${num(sample.buffer_frames / sample.sample_rate * 1000, 2)} мс` : "—"], ["Ввод", snapshot.audio.input_enabled ? "Включён" : "Выключен"], ["Каналы входа / выхода", `${snapshot.audio.input_channels?.map((n) => n + 1).join(", ") || "По умолчанию"} / ${snapshot.audio.output_channels?.map((n) => n + 1).join(", ") || "По умолчанию"}`], ["Input underflow / overflow", `${snapshot.audio.input_underflow} / ${snapshot.audio.input_overflow}`], ["Output underflow / overflow", `${snapshot.audio.output_underflow} / ${snapshot.audio.output_overflow}`], ["Блоки, пропущенные при изменении графа", num(snapshot.audio.gated_blocks, 0)], ["Аудиопотоки / realtime / workgroup", `${snapshot.audio.workers} / ${snapshot.audio.realtime_workers} / ${snapshot.audio.workgroup_workers}`]]} />
      </section>
      <PluginChain title="Эффекты Master" slots={snapshot.master_inserts} trackName={trackName} />
      <section className="vlt-stack"><div className="telemetry-toolbar"><h3 className="vlt-section-title">Дорожки · {tracks.length}</h3><label className="telemetry-search"><span className="sr-only">Найти дорожку или плагин</span><input className="vlt-input" placeholder="Дорожка или плагин…" value={query} onChange={(e) => setQuery(e.target.value)} /></label></div>
        <div className="telemetry-tracks">{visible.map((track, index) => <TrackDisclosure key={track.id || index} summary={<><span><strong>{track.name || "Без названия"}</strong><small>{track.kind} · {track.clip_count} клипов · {track.instrument?.name || "Без инструмента"}</small></span><span className="vlt-muted">{[track.muted && "Mute", track.soloed && "Solo", track.armed && "Rec", track.frozen && "Freeze"].filter(Boolean).join(" · ")}</span></>}><div className="telemetry-track-body vlt-stack">
          <Facts values={[["ID", track.id], ["Родитель", track.parent_id ? trackName(track.parent_id) : "Нет"], ["Выход", trackName(track.output_bus_id)], ["Громкость / панорама", `${num(track.volume)} / ${num(track.pan)}`], ["Вход", track.input_enabled ? `Канал ${track.input_channel + 1} · ${track.input_channel_count} кан.` : "Выключен"], ["Мониторинг / моно", `${track.monitor ? "Вкл." : "Выкл."} / ${track.mono ? "Да" : "Нет"}`]]} />
          <PluginChain title="Инструмент" slots={track.instrument ? [track.instrument] : []} trackName={trackName} /><PluginChain title="Эффекты дорожки" slots={track.inserts} trackName={trackName} /><PluginChain title={`Эффекты сэмплера${track.sampler_fx_active ? "" : " (неактивны)"}`} slots={track.sampler_fx} trackName={trackName} />
          {!!track.clip_fx?.length && <section className="vlt-stack"><h4>Эффекты клипов · {track.clip_fx.length}</h4>{track.clip_fx.map((clip, i) => <details className="telemetry-track" key={clip.id || i}><summary>{clip.name || "Клип без названия"} · {clip.kind} · {num(clip.start_seconds)} с / {num(clip.duration_seconds)} с{clip.muted ? " · Mute" : ""}</summary><div className="telemetry-track-body"><PluginChain title="Эффекты клипа" slots={clip.inserts} trackName={trackName} /><PluginChain title="Офлайн-обработка" slots={clip.offline_inserts} trackName={trackName} /></div></details>)}</section>}
          {!!track.sends?.length && <section><h4>Посылы</h4><ul className="telemetry-sends">{track.sends.map((send, i) => <li key={i}>{trackName(send.destination_track_id)} · {num(send.level * 100)}% · {send.pre_fader ? "Pre-fader" : "Post-fader"} · {send.enabled ? "Включён" : "Выключен"}</li>)}</ul></section>}
        </div></TrackDisclosure>)}</div>{!visible.length && <p className="vlt-muted">{tracks.length ? "Дорожки не найдены." : "В проекте нет дорожек."}</p>}
      </section>
    </> : <><p className="vlt-muted">Старая версия отчёта: подробности дорожек и аудиоустройств не собирались.</p><div className="plugin-chips">{sample.plugins?.map((plugin, i) => <span className="vlt-badge" key={i}>{plugin.format} · {plugin.name} · {plugin.vendor} {plugin.version} × {plugin.count}</span>)}</div></>}
    <details className="telemetry-track"><summary>Система при запуске</summary><pre className="telemetry-json">{JSON.stringify(report.session.hardware ?? {}, null, 2)}</pre></details>
    <details className="telemetry-track"><summary>Исходный JSON отчёта</summary><pre className="telemetry-json">{JSON.stringify(report, null, 2)}</pre></details>
  </div>;
}

function SampleRow({ userID, sample }: { userID: string; sample: Sample }) {
  const [open, setOpen] = useState(false), [report, setReport] = useState<Report>();
  const [error, setError] = useState(""), [loading, setLoading] = useState(false);
  async function load() {
    setLoading(true); setError("");
    try { setReport(await api.request<Report>(`/v1/admin/users/${userID}/telemetry/${sample.event_id}`)); }
    catch (e) { setError((e as Error).message || "Не удалось загрузить снимок."); }
    finally { setLoading(false); }
  }
  return <article className="telemetry-sample"><button className="telemetry-sample-toggle" aria-expanded={open} aria-controls={`sample-${sample.event_id}`} onClick={() => { setOpen(!open); if (!open && !report && !loading) void load(); }}>
    <span aria-hidden="true">{open ? "−" : "+"}</span><span><strong>{date(sample.recorded_at)}</strong><small>{state(sample)} · {sample.track_count} дорожек · {sample.plugin_count} плагинов</small></span><span className="telemetry-sample-metrics">DSP {num(sample.dsp_load)}%<small>{sample.xruns} аудиосбоев</small></span>
  </button>{open && <div id={`sample-${sample.event_id}`} className="telemetry-sample-body">{loading && <p role="status">Загрузка снимка…</p>}{error && <div role="alert" className="vlt-error">{error}<button className="vlt-button vlt-button-secondary" onClick={() => void load()}>Повторить</button></div>}{report && <ReportView report={report} />}</div>}</article>;
}

export function TelemetryHistory({ userID }: { userID: string }) {
  const [items, setItems] = useState<Sample[]>([]), [cursor, setCursor] = useState("");
  const [sessions, setSessions] = useState<Session[]>([]);
  const [error, setError] = useState(""), [loading, setLoading] = useState(true), [moreLoading, setMoreLoading] = useState(false);
  const [updated, setUpdated] = useState("");
  useEffect(() => {
    let active = true, busy = false;
    let initialized = false;
    let newest: Sample | undefined;
    async function refresh() {
      if (busy) return; busy = true;
      try {
        const page = await api.request<TelemetryPage>(`/v1/admin/users/${userID}/telemetry?limit=20`);
        const incoming = [...(page.samples ?? [])];
        let tail = page;
        const visited = new Set<string>();
        // Catch up after an inactive tab or a batch arriving from an offline
        // desktop, without leaving an unreachable gap between loaded pages.
        while (active && newest && tail.next_cursor && tail.samples?.length &&
          compareSamples(tail.samples[tail.samples.length - 1], newest) < 0 && !visited.has(tail.next_cursor)) {
          visited.add(tail.next_cursor);
          tail = await api.request<TelemetryPage>(`/v1/admin/users/${userID}/telemetry?limit=20&before=${encodeURIComponent(tail.next_cursor)}`);
          incoming.push(...(tail.samples ?? []));
        }
        if (!active) return;
        setItems((old) => mergeSamples(old, incoming));
        newest = page.samples?.[0] ?? newest;
        if (!initialized) { setCursor(page.next_cursor || ""); initialized = !!page.samples?.length; }
        setSessions(page.sessions ?? []);
        setUpdated(new Date().toLocaleTimeString("ru")); setError("");
      } catch (e) { if (active) setError((e as Error).message); }
      finally { busy = false; if (active) setLoading(false); }
    }
    void refresh();
    const timer = window.setInterval(() => { if (adminPollingAllowed()) void refresh(); }, 30000);
    return () => { active = false; window.clearInterval(timer); };
  }, [userID]);
  async function more() {
    setMoreLoading(true); setError("");
    try {
      const page = await api.request<TelemetryPage>(`/v1/admin/users/${userID}/telemetry?limit=20&before=${encodeURIComponent(cursor)}`);
      setItems((old) => mergeSamples(old, page.samples ?? [])); setCursor(page.next_cursor || "");
    } catch (e) { setError((e as Error).message); }
    finally { setMoreLoading(false); }
  }
  return <div className="vlt-stack"><p className="vlt-muted">Новый снимок каждые 5 минут, а также при запуске и завершении. Нажмите на запись, чтобы посмотреть состояние программы. При отсутствии сети снимки поступят позже.</p><div className="telemetry-toolbar"><span>{items.length} снимков загружено</span><span className="vlt-muted">{updated ? `Проверено в ${updated}` : "Проверяем…"}</span></div>{error && <p role="alert" className="vlt-error">{error}</p>}{loading && <p role="status">Загрузка истории…</p>}{!loading && !items.length && !error && <p className="vlt-muted">Снимки ещё не поступали.</p>}{!!sessions.length && <details className="telemetry-track"><summary>Последние запуски · {sessions.length}</summary><div className="telemetry-track-body">{sessions.map((session) => <Facts key={session.id} values={[["Запуск", date(session.started_at)], ["Версия / сборка", `${session.app_version} / ${session.build_id}`], ["Завершение", session.ended_at ? `${date(session.ended_at)} · ${session.end_reason || "normal"}` : "Нет отметки о завершении"]]} />)}</div></details>}<div className="telemetry-history">{items.map((sample) => <SampleRow key={sample.event_id} userID={userID} sample={sample} />)}</div>{cursor && <button className="vlt-button vlt-button-secondary" disabled={moreLoading} onClick={() => void more()}>{moreLoading ? "Загрузка…" : "Загрузить более ранние"}</button>}</div>;
}
function mergeSamples(old: Sample[], incoming: Sample[]) {
  return Array.from(new Map([...old, ...incoming].map((sample) => [sample.event_id, sample])).values()).sort(compareSamples);
}

function compareSamples(a: Sample, b: Sample) {
  return Date.parse(b.recorded_at) - Date.parse(a.recorded_at) || b.id.localeCompare(a.id);
}
