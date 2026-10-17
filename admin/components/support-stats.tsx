"use client";

import { useEffect, useState } from "react";
import { api } from "@vlt/api-client";
import { useAdmin } from "./use-admin";

type SupportClick = { source: "home" | "footer"; clicks: number; last_clicked_at: string | null };
const number = new Intl.NumberFormat("ru-RU");

export function SupportStats() {
  const { session } = useAdmin();
  const [rows, setRows] = useState<SupportClick[]>();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [refresh, setRefresh] = useState(0);
  useEffect(() => {
    if (!session) return;
    const controller = new AbortController();
    setBusy(true); setError("");
    api.request<{ clicks: SupportClick[] }>("/v1/admin/dashboard/support-stats", { signal: controller.signal })
      .then(result => { if (!controller.signal.aborted) setRows(result.clicks); })
      .catch(() => { if (!controller.signal.aborted) setError("Не удалось загрузить переходы на Boosty. Попробуйте обновить."); })
      .finally(() => { if (!controller.signal.aborted) setBusy(false); });
    return () => controller.abort();
  }, [session, refresh]);
  return <section className="vlt-card vlt-card-pad support-stats" aria-labelledby="support-stats-title" aria-busy={busy}>
    <div className="vlt-row vlt-between"><h2 className="vlt-section-title" id="support-stats-title">Переходы на Boosty</h2><button type="button" className="vlt-button vlt-button-secondary" disabled={busy || !session} onClick={() => setRefresh(value => value + 1)}>{busy ? "Загрузка…" : "Обновить переходы"}</button></div>
    <p className="vlt-subtitle">Нажатия на ссылки поддержки с момента включения счётчика. Повторные нажатия учитываются. Это не число подписок или уникальных посетителей.</p>
    {error && <p className="vlt-error" role="alert">{error}</p>}
    {rows && <>
      <p className="download-total" aria-live="polite">Всего переходов: <strong>{number.format(rows.reduce((sum, row) => sum + row.clicks, 0))}</strong></p>
      <div className="download-stats-table"><table className="vlt-table"><caption className="sr-only">Переходы на Boosty по расположению ссылки</caption>
        <thead><tr><th scope="col">Ссылка</th><th scope="col">Переходы</th><th scope="col">Последний переход</th></tr></thead>
        <tbody>{rows.map(row => <tr key={row.source}><th scope="row">{row.source === "home" ? "Блок на главной" : "Футер сайта"}</th><td className="download-count">{number.format(row.clicks)}</td><td>{row.last_clicked_at ? new Date(row.last_clicked_at).toLocaleString("ru-RU") : "Пока нет"}</td></tr>)}</tbody>
      </table></div>
    </>}
  </section>;
}
