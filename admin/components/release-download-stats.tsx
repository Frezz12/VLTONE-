"use client";

import { useEffect, useState } from "react";
import { api } from "@vlt/api-client";
import { useAdmin } from "./use-admin";

type Download = { release_id: string; version: string; kind: string; clicks: number; last_clicked_at: string | null };
const formats: Record<string, string> = { "windows-exe": "Windows EXE", "macos-dmg": "macOS DMG", "macos-pkg": "macOS PKG", "linux-appimage": "Linux AppImage", "linux-deb": "Linux DEB", "linux-rpm": "Linux RPM", "linux-tar-gz": "Linux TAR.GZ", "linux-tar-xz": "Linux TAR.XZ" };
const number = new Intl.NumberFormat("ru-RU");

export function ReleaseDownloadStats() {
  const { session } = useAdmin();
  const [rows, setRows] = useState<Download[]>();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  async function load() {
    setBusy(true); setError("");
    try { setRows((await api.request<{ downloads: Download[] }>("/v1/admin/releases/download-stats")).downloads); }
    catch { setError("Не удалось загрузить статистику. Попробуйте обновить."); }
    finally { setBusy(false); }
  }
  useEffect(() => { if (session) void load(); }, [session]);
  const versions = new Map<string, { version: string; clicks: number; formats: Download[] }>();
  for (const row of rows ?? []) {
    const item = versions.get(row.release_id) ?? { version: row.version, clicks: 0, formats: [] };
    item.clicks += row.clicks; item.formats.push(row); versions.set(row.release_id, item);
  }
  return <section className="vlt-card vlt-card-pad release-download-stats" aria-labelledby="download-stats-title" aria-busy={busy}>
    <div className="vlt-row vlt-between"><h2 className="vlt-section-title" id="download-stats-title">Статистика скачиваний</h2><button className="vlt-button vlt-button-secondary" type="button" onClick={() => void load()} disabled={busy || !session}>{busy ? "Загрузка…" : "Обновить статистику"}</button></div>
    <p className="vlt-subtitle">Нажатия «Скачать» на сайте с момента включения статистики. Повторные клики учитываются; это не число уникальных людей или завершённых загрузок.</p>
    {error && <p className="vlt-error" role="alert">{error}</p>}
    {rows && <>
      <p className="download-total">Всего нажатий: <strong>{number.format(rows.reduce((sum, row) => sum + row.clicks, 0))}</strong></p>
      {rows.length === 0 ? <p className="vlt-muted">Пока нет опубликованных версий с установщиками.</p> :
        <div className="download-stats-table"><table className="vlt-table"><caption className="sr-only">Нажатия на скачивание по версиям и форматам</caption><thead><tr><th scope="col">Версия</th><th scope="col">Всего нажатий</th><th scope="col">По форматам</th></tr></thead><tbody>
          {[...versions].map(([id, item]) => <tr key={id}><th scope="row">{item.version}</th><td className="download-count">{number.format(item.clicks)}</td><td><ul className="download-format-list">{item.formats.map(row => <li key={row.kind}><span>{formats[row.kind] ?? row.kind}</span><strong>{number.format(row.clicks)}</strong></li>)}</ul></td></tr>)}
        </tbody></table></div>}
    </>}
  </section>;
}
